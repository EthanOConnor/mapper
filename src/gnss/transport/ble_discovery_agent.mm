/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#if !__has_feature(objc_arc)
#error "This file requires ARC (-fobjc-arc)"
#endif

#include "ble_discovery_agent.h"

#import <CoreBluetooth/CoreBluetooth.h>

#include <QString>

#include "ble_device_model.h"
#include "ios_ble_nus_transport.h"


// NUS service UUID for scan filter
static CBUUID* nusServiceUuid()
{
	static CBUUID* uuid = [CBUUID UUIDWithString:@"6E400001-B5A3-F393-E0A9-E50E24DCCA9E"];
	return uuid;
}


// ============================================================================
// BleDiscoveryDelegate — CBCentralManagerDelegate for scan-only discovery
// ============================================================================

@interface BleDiscoveryDelegate : NSObject <CBCentralManagerDelegate>
{
	OpenOrienteering::BleDiscoveryAgent* _agent;
	CBCentralManager* _centralManager;
	NSMutableDictionary<NSString*, CBPeripheral*>* _discoveredPeripherals;
}
- (instancetype)initWithAgent:(OpenOrienteering::BleDiscoveryAgent*)agent;
- (void)startScan;
- (void)stopScan;
- (CBPeripheral*)cachedPeripheralForUuid:(NSString*)uuidString;
- (CBCentralManager*)detachCentralManager;
- (void)teardown;
@end


// Guard: bail from callback if C++ agent is already torn down
#define GUARD_AGENT() do { if (!_agent) return; } while(0)


@implementation BleDiscoveryDelegate

- (instancetype)initWithAgent:(OpenOrienteering::BleDiscoveryAgent*)agent
{
	self = [super init];
	if (self) {
		_agent = agent;
		_discoveredPeripherals = [NSMutableDictionary new];
		_centralManager = [[CBCentralManager alloc] initWithDelegate:self
		                                                       queue:nil
		                                                     options:nil];
	}
	return self;
}

- (void)startScan
{
	if (_centralManager.state == CBManagerStatePoweredOn) {
		NSLog(@"GNSS Discovery: starting scan for NUS devices");
		[_centralManager scanForPeripheralsWithServices:@[nusServiceUuid()]
		                                       options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
	}
}

- (void)stopScan
{
	[_centralManager stopScan];
}

- (CBPeripheral*)cachedPeripheralForUuid:(NSString*)uuidString
{
	return _discoveredPeripherals[uuidString];
}

- (CBCentralManager*)detachCentralManager
{
	// Stop scanning before handoff
	[_centralManager stopScan];

	// Detach: nil the agent back-pointer so no more callbacks reach C++
	_agent = nil;

	// Remove ourselves as delegate — the transport will set its own delegate
	_centralManager.delegate = nil;

	// Return the manager (caller takes ownership via strong ref)
	CBCentralManager* manager = _centralManager;
	_centralManager = nil;
	return manager;
}

- (void)teardown
{
	_agent = nil;
	[_centralManager stopScan];
	_centralManager.delegate = nil;
	_centralManager = nil;
	[_discoveredPeripherals removeAllObjects];
}


// ---- CBCentralManagerDelegate ----

- (void)centralManagerDidUpdateState:(CBCentralManager*)central
{
	NSLog(@"GNSS Discovery: centralManagerDidUpdateState: %ld", (long)central.state);
	GUARD_AGENT();
	_agent->didUpdateState(static_cast<int>(central.state));
}

- (void)centralManager:(CBCentralManager*)central
 didDiscoverPeripheral:(CBPeripheral*)peripheral
     advertisementData:(NSDictionary<NSString*,id>*)advertisementData
                  RSSI:(NSNumber*)RSSI
{
	GUARD_AGENT();

	NSString* name = peripheral.name;
	if (!name || name.length == 0)
		name = advertisementData[CBAdvertisementDataLocalNameKey];
	if (!name || name.length == 0)
		name = @"Unknown GNSS";

	// Cache the exact CBPeripheral instance — critical for ArduSimple.
	// retrievePeripheralsWithIdentifiers returns a different object that
	// may fail to connect on some firmware.
	_discoveredPeripherals[peripheral.identifier.UUIDString] = peripheral;

	_agent->didDiscoverDevice(
		QString::fromNSString(name),
		QString::fromNSString(peripheral.identifier.UUIDString),
		RSSI.intValue
	);
}

@end


// ============================================================================
// BleDiscoveryAgent — C++ implementation
// ============================================================================

// CBManagerState value forwarded as int to C++ callback
static constexpr int kCBManagerStatePoweredOn = 5;

namespace OpenOrienteering {


BleDiscoveryAgent::BleDiscoveryAgent(BleDeviceModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
}

BleDiscoveryAgent::~BleDiscoveryAgent()
{
	if (m_delegate) {
		[m_delegate teardown];
		m_delegate = nullptr;
	}
}


void BleDiscoveryAgent::startScan()
{
	if (m_scanning)
		return;

	m_model->clearDiscovered();

	if (!m_delegate) {
		m_delegate = [[BleDiscoveryDelegate alloc] initWithAgent:this];
		// CBCentralManager may not be powered on yet — defer scan
		m_scanPending = true;
		return;
	}

	[m_delegate startScan];
	m_scanning = true;
	emit scanningChanged(true);
}

void BleDiscoveryAgent::stopScan()
{
	m_scanPending = false;
	if (m_delegate)
		[m_delegate stopScan];

	if (m_scanning) {
		m_scanning = false;
		emit scanningChanged(false);
	}
}

bool BleDiscoveryAgent::isScanning() const
{
	return m_scanning;
}


std::unique_ptr<GnssTransport> BleDiscoveryAgent::createTransportForDevice(
    const QString& uuid, const QString& deviceName)
{
	if (!m_delegate)
		return nullptr;

	// 1. Stop scanning
	[m_delegate stopScan];

	// 2. Get the exact CBPeripheral from scan cache
	CBPeripheral* peripheral = [m_delegate cachedPeripheralForUuid:uuid.toNSString()];
	if (!peripheral) {
		qWarning("GNSS Discovery: peripheral %s not found in cache", qPrintable(uuid));
		return nullptr;
	}

	// 3. Detach CBCentralManager from discovery delegate
	//    (stops scan, nils agent back-pointer, nils delegate on manager)
	CBCentralManager* manager = [m_delegate detachCentralManager];

	// 4. Agent is now inert
	m_delegate = nullptr;
	if (m_scanning) {
		m_scanning = false;
		emit scanningChanged(false);
	}

	// 5. Create transport — takes ownership of manager + peripheral
	return std::make_unique<IosBleNusTransport>(
	    manager, peripheral, uuid, deviceName, /*parent=*/nullptr);
}


// ---- Callbacks from ObjC delegate ----

void BleDiscoveryAgent::didUpdateState(int state)
{
	if (state == kCBManagerStatePoweredOn && m_scanPending) {
		m_scanPending = false;
		[m_delegate startScan];
		m_scanning = true;
		emit scanningChanged(true);
	}
}

void BleDiscoveryAgent::didDiscoverDevice(const QString& name, const QString& uuid, int rssi)
{
	BleDeviceInfo info;
	info.name = name;
	info.address = uuid;
	info.rssi = rssi;
	m_model->addOrUpdate(info);
}


}  // namespace OpenOrienteering
