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


// ============================================================================
// BleScannerDelegate — single CBCentralManager for scan + connect + data
// ============================================================================

@interface BleScannerDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
{
	OpenOrienteering::BleScannerCoreBluetooth* _scanner;
	CBCentralManager* _centralManager;
	CBPeripheral* _peripheral;
	CBCharacteristic* _rxCharacteristic;
	CBCharacteristic* _txCharacteristic;
	NSUUID* _connectTarget;
}
- (instancetype)initWithScanner:(OpenOrienteering::BleScannerCoreBluetooth*)scanner;
- (void)startScan;
- (void)stopScan;
- (void)connectToUuid:(NSString*)uuidString;
- (BOOL)writeData:(const QByteArray&)data;
@end


@implementation BleScannerDelegate

- (instancetype)initWithScanner:(OpenOrienteering::BleScannerCoreBluetooth*)scanner
{
	self = [super init];
	if (self) {
		_scanner = scanner;
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
	_connectTarget = [[NSUUID alloc] initWithUUIDString:uuidString];

	NSArray<CBPeripheral*>* known = [_centralManager
		retrievePeripheralsWithIdentifiers:@[_connectTarget]];

	NSLog(@"GNSS BLE: connectToUuid, retrieved %lu peripherals", (unsigned long)known.count);

	if (known.count > 0) {
		_peripheral = known.firstObject;
		_peripheral.delegate = self;
		NSLog(@"GNSS BLE: connecting to %@ (%@)", _peripheral.name, _peripheral.identifier);
		[_centralManager connectPeripheral:_peripheral options:nil];
	} else {
		NSLog(@"GNSS BLE: peripheral not found, cannot connect");
		_scanner->didFailToConnect(
			QString::fromNSString(@"Device no longer available"));
	}
}

- (BOOL)writeData:(const QByteArray&)data
{
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
	_scanner->didConnect();
	[peripheral discoverServices:nil];  // nil = discover all services
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
	_scanner->didDisconnect(
		QString::fromNSString(error ? error.localizedDescription : @""));
	_rxCharacteristic = nil;
	_txCharacteristic = nil;
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
	for (CBService* service in peripheral.services) {
		NSLog(@"GNSS BLE:   service: %@", service.UUID);
		if ([service.UUID isEqual:nusServiceUuid()]) {
			foundNus = YES;
			NSLog(@"GNSS BLE: found NUS — discovering characteristics");
			[peripheral discoverCharacteristics:@[nusRxCharUuid(), nusTxCharUuid()]
			                         forService:service];
		}
	}
	if (!foundNus) {
		NSLog(@"GNSS BLE: NUS service NOT found");
		_scanner->didFailToConnect(
			QString::fromNSString(@"NUS service not found on this device"));
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
			_rxCharacteristic = ch;
		} else if ([ch.UUID isEqual:nusTxCharUuid()]) {
			_txCharacteristic = ch;
			NSLog(@"GNSS BLE: subscribing to TX notifications");
			[peripheral setNotifyValue:YES forCharacteristic:ch];
		}
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
