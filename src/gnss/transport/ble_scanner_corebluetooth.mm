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


#include "ble_scanner_corebluetooth.h"

#import <CoreBluetooth/CoreBluetooth.h>

#include <QByteArray>
#include <QString>
#include <QTimer>

#include "ble_device_model.h"


static CBUUID* nusServiceUuid()
{
	static CBUUID* uuid = [CBUUID UUIDWithString:@"6E400001-B5A3-F393-E0A9-E50E24DCCA9E"];
	return uuid;
}

static CBUUID* nusRxCharUuid()
{
	static CBUUID* uuid = [CBUUID UUIDWithString:@"6E400002-B5A3-F393-E0A9-E50E24DCCA9E"];
	return uuid;
}

static CBUUID* nusTxCharUuid()
{
	static CBUUID* uuid = [CBUUID UUIDWithString:@"6E400003-B5A3-F393-E0A9-E50E24DCCA9E"];
	return uuid;
}

// Custom GNSS Hub service — our future custom firmware advertises this
// alongside NUS for backward compatibility. Contains L2CAP PSM characteristic.
static CBUUID* gnssHubServiceUuid()
{
	// Custom UUID for our GNSS Hub firmware
	static CBUUID* uuid = [CBUUID UUIDWithString:@"00000001-6E55-4800-0000-00000000CAFE"];
	return uuid;
}

static CBUUID* gnssL2capPsmCharUuid()
{
	// Characteristic that holds the L2CAP PSM number (uint16, read)
	static CBUUID* uuid = [CBUUID UUIDWithString:@"00000002-6E55-4800-0000-00000000CAFE"];
	return uuid;
}

// Well-known PSM for GNSS Hub L2CAP channel (used if GATT lookup fails)
static const CBL2CAPPSM kGnssHubL2capPsm = 0x0080;

// Max connect attempts (initial + retries)
static const int kMaxConnectAttempts = 4;


// ============================================================================
// BleScannerDelegate — single CBCentralManager for scan + connect + data
// ============================================================================

@interface BleScannerDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate, NSStreamDelegate>
{
	OpenOrienteering::BleScannerCoreBluetooth* _scanner;
	CBCentralManager* _centralManager;
	CBPeripheral* _peripheral;
	CBCharacteristic* _rxCharacteristic;
	CBCharacteristic* _txCharacteristic;
	CBL2CAPChannel* _l2capChannel;
	NSUUID* _connectTarget;
	NSTimer* _connectTimeoutTimer;
	NSMutableDictionary<NSString*, CBPeripheral*>* _discoveredPeripherals;
	BOOL _connected;
	BOOL _usingL2cap;
	int _connectAttempt;
}
- (instancetype)initWithScanner:(OpenOrienteering::BleScannerCoreBluetooth*)scanner;
- (void)startScan;
- (void)stopScan;
- (void)connectToUuid:(NSString*)uuidString;
- (void)cancelConnection;
- (BOOL)writeData:(const QByteArray&)data;
@end


@implementation BleScannerDelegate

- (instancetype)initWithScanner:(OpenOrienteering::BleScannerCoreBluetooth*)scanner
{
	self = [super init];
	if (self) {
		_scanner = scanner;
		_discoveredPeripherals = [NSMutableDictionary new];
		_centralManager = [[CBCentralManager alloc] initWithDelegate:self queue:nil options:nil];
	}
	return self;
}

- (void)startScan
{
	if (_centralManager.state == CBManagerStatePoweredOn) {
		NSLog(@"GNSS BLE: starting scan for NUS devices");
		[_centralManager scanForPeripheralsWithServices:nil
		                                       options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
	}
}

- (void)stopScan
{
	[_centralManager stopScan];
}

- (void)connectToUuid:(NSString*)uuidString
{
	[_centralManager stopScan];
	[self cancelConnection];

	_connected = NO;
	_connectAttempt = 1;
	_connectTarget = [[NSUUID alloc] initWithUUIDString:uuidString];

	// Use the EXACT CBPeripheral object from scan discovery — this is critical.
	// retrievePeripheralsWithIdentifiers returns a different object that may
	// fail to connect on some devices (e.g., ArduSimple bridge).
	CBPeripheral* discovered = _discoveredPeripherals[uuidString];
	if (discovered) {
		_peripheral = discovered;
		_peripheral.delegate = self;
		NSLog(@"GNSS BLE: connecting to discovered peripheral %@ (%@)", _peripheral.name, _peripheral.identifier);
		[_centralManager connectPeripheral:_peripheral options:nil];

		// CoreBluetooth connectPeripheral has NO timeout — it waits forever.
		// We must cancel and retry ourselves.
		_connectTimeoutTimer = [NSTimer scheduledTimerWithTimeInterval:3.0
			target:self
			selector:@selector(connectTimeout:)
			userInfo:nil
			repeats:NO];
	} else {
		// Fallback to retrieve (shouldn't normally happen)
		NSArray<CBPeripheral*>* known = [_centralManager
			retrievePeripheralsWithIdentifiers:@[_connectTarget]];
		NSLog(@"GNSS BLE: peripheral not in scan cache, retrieved %lu from system", (unsigned long)known.count);
		if (known.count > 0) {
			_peripheral = known.firstObject;
			_peripheral.delegate = self;
			[_centralManager connectPeripheral:_peripheral options:nil];
			_connectTimeoutTimer = [NSTimer scheduledTimerWithTimeInterval:3.0
				target:self selector:@selector(connectTimeout:)
				userInfo:nil repeats:NO];
		} else {
			_scanner->didFailToConnect(
				QString::fromNSString(@"Device no longer available"));
		}
	}
}

- (void)connectTimeout:(NSTimer*)timer
{
	_connectTimeoutTimer = nil;
	if (_connected)
		return;

	NSLog(@"GNSS BLE: connect timeout — canceling and retrying");
	// Cancel the pending connection without triggering didDisconnect
	// (cancelPeripheralConnection can trigger the disconnect delegate,
	// which would emit deviceConnectionFailed to the UI)
	if (_peripheral) {
		[_centralManager cancelPeripheralConnection:_peripheral];
		_peripheral = nil;
	}

	_connectAttempt++;
	if (_connectAttempt > kMaxConnectAttempts) {
		NSLog(@"GNSS BLE: all %d connect attempts exhausted", kMaxConnectAttempts);
		_scanner->didFailToConnect(
			QString::fromNSString(@"Connection timed out after multiple attempts"));
		return;
	}

	// Retry directly from here rather than going through the C++ retry path,
	// to avoid race conditions between ObjC callbacks and Qt signals
	NSArray<CBPeripheral*>* known = [_centralManager
		retrievePeripheralsWithIdentifiers:@[_connectTarget]];
	if (known.count > 0) {
		NSLog(@"GNSS BLE: timeout — attempt %d of %d", _connectAttempt, kMaxConnectAttempts);
		_scanner->didRetryConnect(_connectAttempt, kMaxConnectAttempts);
		_peripheral = known.firstObject;
		_peripheral.delegate = self;
		[_centralManager connectPeripheral:_peripheral options:nil];
		_connectTimeoutTimer = [NSTimer scheduledTimerWithTimeInterval:3.0
			target:self selector:@selector(connectTimeout:)
			userInfo:nil repeats:NO];
	} else {
		NSLog(@"GNSS BLE: timeout retry — peripheral lost");
		_scanner->didFailToConnect(
			QString::fromNSString(@"Connection timed out, device not found"));
	}
}

- (void)cancelConnection
{
	[_connectTimeoutTimer invalidate];
	_connectTimeoutTimer = nil;
	if (_peripheral && !_connected) {
		[_centralManager cancelPeripheralConnection:_peripheral];
	}
}

- (BOOL)writeData:(const QByteArray&)data
{
	// Prefer L2CAP if available (higher throughput, no fragmentation needed)
	if (_usingL2cap && _l2capChannel) {
		NSInteger written = [_l2capChannel.outputStream write:
			reinterpret_cast<const uint8_t*>(data.constData())
			maxLength:data.size()];
		if (written > 0) {
			_scanner->didWriteData(static_cast<int>(written));
			return written == data.size();
		}
		// L2CAP write failed — fall through to NUS
		NSLog(@"GNSS BLE: L2CAP write failed, falling back to NUS");
	}

	// NUS GATT write path
	if (!_rxCharacteristic || !_peripheral)
		return NO;

	CBCharacteristicWriteType writeType =
		(_rxCharacteristic.properties & CBCharacteristicPropertyWriteWithoutResponse)
			? CBCharacteristicWriteWithoutResponse
			: CBCharacteristicWriteWithResponse;

	NSInteger mtu = [_peripheral maximumWriteValueLengthForType:writeType];
	if (mtu <= 0)
		mtu = 20;

	const char* bytes = data.constData();
	NSInteger remaining = data.size();
	NSInteger offset = 0;
	int totalWritten = 0;

	while (remaining > 0) {
		NSInteger chunkSize = MIN(remaining, mtu);
		NSData* chunk = [NSData dataWithBytes:(bytes + offset) length:chunkSize];
		[_peripheral writeValue:chunk forCharacteristic:_rxCharacteristic type:writeType];
		offset += chunkSize;
		remaining -= chunkSize;
		totalWritten += chunkSize;
	}

	if (totalWritten > 0)
		_scanner->didWriteData(totalWritten);

	return YES;
}


// ---- CBCentralManagerDelegate ----

- (void)centralManagerDidUpdateState:(CBCentralManager*)central
{
	NSLog(@"GNSS BLE: centralManagerDidUpdateState: %ld", (long)central.state);
	_scanner->didUpdateState(static_cast<int>(central.state));
}

- (void)centralManager:(CBCentralManager*)central
 didDiscoverPeripheral:(CBPeripheral*)peripheral
     advertisementData:(NSDictionary<NSString*,id>*)advertisementData
                  RSSI:(NSNumber*)RSSI
{
	NSString* name = peripheral.name;
	if (!name || name.length == 0)
		name = advertisementData[CBAdvertisementDataLocalNameKey];
	if (!name || name.length == 0)
		name = @"Unknown GNSS";

	// Store the actual CBPeripheral object — we MUST connect to this exact instance,
	// not a new one from retrievePeripheralsWithIdentifiers (which can fail to connect)
	_discoveredPeripherals[peripheral.identifier.UUIDString] = peripheral;

	_scanner->didDiscoverDevice(
		QString::fromNSString(name),
		QString::fromNSString(peripheral.identifier.UUIDString),
		RSSI.intValue
	);
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral
{
	NSLog(@"GNSS BLE: didConnectPeripheral: %@ — discovering ALL services", peripheral.name);
	_connected = YES;
	[_connectTimeoutTimer invalidate];
	_connectTimeoutTimer = nil;
	_scanner->didConnect();
	// Discover ALL services — some bridges don't advertise NUS in filtered discovery
	[peripheral discoverServices:nil];
}

- (void)centralManager:(CBCentralManager*)central
didFailToConnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
	NSLog(@"GNSS BLE: didFailToConnect: %@", error);
	_scanner->didFailToConnect(
		QString::fromNSString(error ? error.localizedDescription : @"Connection failed"));
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
	NSLog(@"GNSS BLE: didDisconnect: %@", error);
	_rxCharacteristic = nil;
	_txCharacteristic = nil;

	// Only notify C++ if this was a real disconnect (not our timeout cancel)
	if (_connected) {
		_connected = NO;
		_scanner->didDisconnect(
			QString::fromNSString(error ? error.localizedDescription : @""));
	}
	// If not connected yet, the timeout handler manages retries
}


// ---- CBPeripheralDelegate ----

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverServices:(NSError*)error
{
	if (error) {
		NSLog(@"GNSS BLE: service discovery error: %@", error);
		_scanner->didFailToConnect(QString::fromNSString(error.localizedDescription));
		return;
	}

	NSLog(@"GNSS BLE: discovered %lu services:", (unsigned long)peripheral.services.count);
	BOOL foundNus = NO;
	BOOL foundGnssHub = NO;
	for (CBService* service in peripheral.services) {
		NSLog(@"GNSS BLE:   service: %@", service.UUID);
		if ([service.UUID isEqual:nusServiceUuid()]) {
			foundNus = YES;
			NSLog(@"GNSS BLE: found NUS — discovering characteristics");
			[peripheral discoverCharacteristics:@[nusRxCharUuid(), nusTxCharUuid()]
			                         forService:service];
		}
		if ([service.UUID isEqual:gnssHubServiceUuid()]) {
			foundGnssHub = YES;
			NSLog(@"GNSS BLE: found GNSS Hub service — discovering L2CAP PSM characteristic");
			[peripheral discoverCharacteristics:@[gnssL2capPsmCharUuid()]
			                         forService:service];
		}
	}
	if (!foundNus && !foundGnssHub) {
		NSLog(@"GNSS BLE: no compatible service found");
		_scanner->didFailToConnect(
			QString::fromNSString(@"No compatible GNSS service found on this device"));
	}
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service
             error:(NSError*)error
{
	if (error) {
		NSLog(@"GNSS BLE: characteristic discovery error: %@", error);
		_scanner->didFailToConnect(QString::fromNSString(error.localizedDescription));
		return;
	}

	NSLog(@"GNSS BLE: discovered %lu characteristics for service %@",
	      (unsigned long)service.characteristics.count, service.UUID);
	for (CBCharacteristic* ch in service.characteristics) {
		NSLog(@"GNSS BLE:   char: %@ props=%lu", ch.UUID, (unsigned long)ch.properties);
		if ([ch.UUID isEqual:nusRxCharUuid()]) {
			NSLog(@"GNSS BLE: matched NUS RX by UUID");
			_rxCharacteristic = ch;
		} else if ([ch.UUID isEqual:nusTxCharUuid()]) {
			NSLog(@"GNSS BLE: matched NUS TX by UUID — subscribing");
			_txCharacteristic = ch;
			[peripheral setNotifyValue:YES forCharacteristic:ch];
		} else if ([ch.UUID isEqual:gnssL2capPsmCharUuid()]) {
			NSLog(@"GNSS BLE: found L2CAP PSM characteristic — reading");
			[peripheral readValueForCharacteristic:ch];
		}
	}

	// Fallback: if UUID matching failed, match by properties
	// NUS RX: Write or WriteWithoutResponse (props & 0x0C)
	// NUS TX: Notify (props & 0x10)
	if (!_rxCharacteristic || !_txCharacteristic) {
		NSLog(@"GNSS BLE: UUID match failed, trying property-based matching");
		for (CBCharacteristic* ch in service.characteristics) {
			if (!_txCharacteristic && (ch.properties & CBCharacteristicPropertyNotify)) {
				NSLog(@"GNSS BLE: matched TX by Notify property: %@", ch.UUID);
				_txCharacteristic = ch;
				[peripheral setNotifyValue:YES forCharacteristic:ch];
			} else if (!_rxCharacteristic && (ch.properties & CBCharacteristicPropertyWrite)) {
				NSLog(@"GNSS BLE: matched RX by Write property: %@", ch.UUID);
				_rxCharacteristic = ch;
			}
		}
	}

	if ([service.UUID isEqual:gnssHubServiceUuid()] && !_l2capChannel) {
		NSLog(@"GNSS BLE: attempting L2CAP channel with default PSM 0x%04X", kGnssHubL2capPsm);
		[peripheral openL2CAPChannel:kGnssHubL2capPsm];
	}
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
	if (error) {
		NSLog(@"GNSS BLE: notification subscribe error: %@", error);
		return;
	}

	NSLog(@"GNSS BLE: notification state updated, notifying=%d", characteristic.isNotifying);
	if (characteristic.isNotifying) {
		NSInteger mtu = [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];
		NSLog(@"GNSS BLE: ready! MTU=%ld", (long)mtu);
		_scanner->didNegotiateMtu(static_cast<int>(mtu));
		_scanner->didBecomeReady();
	}
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
	if (error || !characteristic.value)
		return;

	NSData* value = characteristic.value;
	QByteArray data(reinterpret_cast<const char*>(value.bytes),
	                static_cast<int>(value.length));
	_scanner->didReceiveData(data);
}

- (void)peripheral:(CBPeripheral*)peripheral
didWriteValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
	if (error) {
		NSLog(@"GNSS BLE: write error: %@", error);
	}
}


// ---- L2CAP Channel Support ----

- (void)peripheral:(CBPeripheral*)peripheral
 didOpenL2CAPChannel:(CBL2CAPChannel*)channel
               error:(NSError*)error
{
	if (error || !channel) {
		NSLog(@"GNSS BLE: L2CAP channel open failed: %@ — falling back to NUS", error);
		// NUS is already set up, so we're fine
		return;
	}

	NSLog(@"GNSS BLE: L2CAP channel opened! PSM=%u", (unsigned)channel.PSM);
	_l2capChannel = channel;
	_usingL2cap = YES;

	// Set up stream reading on the L2CAP channel
	channel.inputStream.delegate = (id<NSStreamDelegate>)self;
	[channel.inputStream scheduleInRunLoop:[NSRunLoop mainRunLoop]
	                               forMode:NSDefaultRunLoopMode];

	_scanner->didNegotiateMtu(0);  // L2CAP doesn't have MTU in the same sense
	NSLog(@"GNSS BLE: now using L2CAP for data transfer (higher throughput)");
}

// NSStreamDelegate for L2CAP inputStream
- (void)stream:(NSStream*)stream handleEvent:(NSStreamEvent)eventCode
{
	if (eventCode == NSStreamEventHasBytesAvailable && stream == _l2capChannel.inputStream) {
		uint8_t buffer[4096];
		NSInteger bytesRead = [_l2capChannel.inputStream read:buffer maxLength:sizeof(buffer)];
		if (bytesRead > 0) {
			QByteArray data(reinterpret_cast<const char*>(buffer),
			                static_cast<int>(bytesRead));
			_scanner->didReceiveData(data);
		}
	} else if (eventCode == NSStreamEventErrorOccurred) {
		NSLog(@"GNSS BLE: L2CAP stream error: %@", stream.streamError);
		_usingL2cap = NO;
		_l2capChannel = nil;
		// Fall back to NUS — it should still be active
	}
}

@end


// ============================================================================
// BleScannerCoreBluetooth — C++ implementation
// ============================================================================

namespace OpenOrienteering {


BleScannerCoreBluetooth::BleScannerCoreBluetooth(BleDeviceModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
}

BleScannerCoreBluetooth::~BleScannerCoreBluetooth()
{
	stopScan();
}


void BleScannerCoreBluetooth::startScan()
{
	if (m_scanning)
		return;

	m_model->clearDiscovered();

	if (!m_delegate) {
		m_delegate = [[BleScannerDelegate alloc] initWithScanner:this];
		m_scanPending = true;
		return;
	}

	[m_delegate startScan];
	m_scanning = true;
	emit scanningChanged(true);
}

void BleScannerCoreBluetooth::stopScan()
{
	m_scanPending = false;
	if (m_delegate)
		[m_delegate stopScan];

	if (m_scanning) {
		m_scanning = false;
		emit scanningChanged(false);
	}
}

void BleScannerCoreBluetooth::connectToDevice(const QString& uuid, const QString& name)
{
	m_connectedDeviceName = name;
	m_connectUuid = uuid;
	m_connectRetries = 0;
	if (m_delegate)
		[m_delegate connectToUuid:uuid.toNSString()];
}

bool BleScannerCoreBluetooth::writeData(const QByteArray& data)
{
	if (!m_delegate)
		return false;
	return [m_delegate writeData:data];
}


// ---- Callbacks from ObjC ----

void BleScannerCoreBluetooth::didDiscoverDevice(const QString& name, const QString& uuid, int rssi)
{
	BleDeviceInfo info;
	info.name = name;
	info.address = uuid;
	info.rssi = rssi;
	m_model->addOrUpdate(info);
}

void BleScannerCoreBluetooth::didUpdateState(int state)
{
	if (state == 5 && m_scanPending) {  // CBManagerStatePoweredOn
		m_scanPending = false;
		[m_delegate startScan];
		m_scanning = true;
		emit scanningChanged(true);
	}
}

void BleScannerCoreBluetooth::didConnect()
{
	// Peripheral connected, service discovery in progress
}

void BleScannerCoreBluetooth::didRetryConnect(int attempt, int maxAttempts)
{
	emit connectionRetrying(attempt, maxAttempts);
}

void BleScannerCoreBluetooth::didFailToConnect(const QString& error)
{
	if (m_connectRetries < kMaxConnectRetries && !m_connectUuid.isEmpty())
	{
		m_connectRetries++;
		qWarning("GNSS BLE: connection attempt %d failed (%s), retrying...",
		         m_connectRetries, qPrintable(error));
		// Retry after a short delay
		QTimer::singleShot(500, this, [this]() {
			if (m_delegate && !m_connectUuid.isEmpty())
				[m_delegate connectToUuid:m_connectUuid.toNSString()];
		});
	}
	else
	{
		emit deviceConnectionFailed(error);
	}
}

void BleScannerCoreBluetooth::didDisconnect(const QString& error)
{
	if (!error.isEmpty())
		emit deviceConnectionFailed(error);
}

void BleScannerCoreBluetooth::didBecomeReady()
{
	emit deviceConnected(m_connectedDeviceName);
}

void BleScannerCoreBluetooth::didReceiveData(const QByteArray& data)
{
	emit dataReceived(data);
}

void BleScannerCoreBluetooth::didWriteData(int bytesWritten)
{
	emit writeComplete(bytesWritten);
}

void BleScannerCoreBluetooth::didNegotiateMtu(int mtu)
{
	m_negotiatedMtu = mtu;
}


}  // namespace OpenOrienteering
