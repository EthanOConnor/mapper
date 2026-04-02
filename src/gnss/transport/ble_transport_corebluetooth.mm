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


#include "ble_transport_corebluetooth.h"

#import <CoreBluetooth/CoreBluetooth.h>

#include <QByteArray>
#include <QString>


// Nordic UART Service UUIDs
static CBUUID* NUS_SERVICE_UUID;
static CBUUID* NUS_RX_CHAR_UUID;  // Phone → receiver (write)
static CBUUID* NUS_TX_CHAR_UUID;  // Receiver → phone (notify)

__attribute__((constructor))
static void initUUIDs() {
	NUS_SERVICE_UUID = [CBUUID UUIDWithString:@"6E400001-B5A3-F393-E0A9-E50E24DCCA9E"];
	NUS_RX_CHAR_UUID = [CBUUID UUIDWithString:@"6E400002-B5A3-F393-E0A9-E50E24DCCA9E"];
	NUS_TX_CHAR_UUID = [CBUUID UUIDWithString:@"6E400003-B5A3-F393-E0A9-E50E24DCCA9E"];
}


// ============================================================================
// BleCoreBtDelegate — ObjC class bridging CoreBluetooth to C++ transport
// ============================================================================

@interface BleCoreBtDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
{
	OpenOrienteering::BleTransportCoreBluetooth* _transport;
	CBCentralManager* _centralManager;
	CBPeripheral* _peripheral;
	CBCharacteristic* _rxCharacteristic;
	CBCharacteristic* _txCharacteristic;
	NSUUID* _targetUuid;
	BOOL _connectPending;
}
- (instancetype)initWithTransport:(OpenOrienteering::BleTransportCoreBluetooth*)transport
                   peripheralUuid:(NSString*)uuidString;
- (void)startConnection;
- (void)disconnect;
- (BOOL)writeData:(const QByteArray&)data;
- (void)dealloc;
@end


@implementation BleCoreBtDelegate

- (instancetype)initWithTransport:(OpenOrienteering::BleTransportCoreBluetooth*)transport
                   peripheralUuid:(NSString*)uuidString
{
	self = [super init];
	if (self) {
		_transport = transport;
		_targetUuid = [[NSUUID alloc] initWithUUIDString:uuidString];
		_connectPending = NO;

		// Create CBCentralManager with state restoration key
		NSDictionary* options = @{
			CBCentralManagerOptionRestoreIdentifierKey: @"org.openorienteering.mapper.gnss"
		};
		_centralManager = [[CBCentralManager alloc] initWithDelegate:self
		                                                       queue:nil
		                                                     options:options];
	}
	return self;
}

- (void)dealloc
{
	if (_peripheral) {
		[_centralManager cancelPeripheralConnection:_peripheral];
	}
}


- (void)startConnection
{
	NSLog(@"GNSS BLE: startConnection, central state=%ld", (long)_centralManager.state);
	if (_centralManager.state == CBManagerStatePoweredOn) {
		[self connectToKnownPeripheral];
	} else {
		NSLog(@"GNSS BLE: central not powered on, deferring connection");
		_connectPending = YES;
	}
}

- (void)connectToKnownPeripheral
{
	NSLog(@"GNSS BLE: connectToKnownPeripheral, target=%@", _targetUuid);
	// Try to retrieve the peripheral by UUID (previously discovered)
	NSArray<CBPeripheral*>* known = [_centralManager
		retrievePeripheralsWithIdentifiers:@[_targetUuid]];

	NSLog(@"GNSS BLE: retrievePeripherals returned %lu devices", (unsigned long)known.count);
	if (known.count > 0) {
		_peripheral = known.firstObject;
		_peripheral.delegate = self;
		NSLog(@"GNSS BLE: connecting to %@ (%@)", _peripheral.name, _peripheral.identifier);
		[_centralManager connectPeripheral:_peripheral options:nil];
	} else {
		NSLog(@"GNSS BLE: peripheral not cached, scanning for NUS service");
		// Peripheral not known — scan for it
		[_centralManager scanForPeripheralsWithServices:@[NUS_SERVICE_UUID]
		                                       options:nil];
	}
}

- (void)disconnect
{
	_connectPending = NO;
	[_centralManager stopScan];
	if (_peripheral) {
		[_centralManager cancelPeripheralConnection:_peripheral];
		_peripheral = nil;
	}
	_rxCharacteristic = nil;
	_txCharacteristic = nil;
}

- (BOOL)writeData:(const QByteArray&)data
{
	if (!_rxCharacteristic || !_peripheral)
		return NO;

	// Determine write type for throughput
	CBCharacteristicWriteType writeType =
		(_rxCharacteristic.properties & CBCharacteristicPropertyWriteWithoutResponse)
			? CBCharacteristicWriteWithoutResponse
			: CBCharacteristicWriteWithResponse;

	// MTU-aware chunking
	NSInteger mtu = [_peripheral maximumWriteValueLengthForType:writeType];
	if (mtu <= 0)
		mtu = 20;  // BLE 4.0 default

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
		_transport->didWriteData(totalWritten);

	return YES;
}


// ---- CBCentralManagerDelegate ----

- (void)centralManagerDidUpdateState:(CBCentralManager*)central
{
	NSLog(@"GNSS BLE: centralManagerDidUpdateState: %ld", (long)central.state);
	_transport->didUpdateState(static_cast<int>(central.state));

	if (central.state == CBManagerStatePoweredOn && _connectPending) {
		_connectPending = NO;
		[self connectToKnownPeripheral];
	}
}

- (void)centralManager:(CBCentralManager*)central
 willRestoreState:(NSDictionary<NSString*,id>*)dict
{
	// State restoration — reconnect to previously connected peripheral
	NSArray<CBPeripheral*>* peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey];
	if (peripherals.count > 0) {
		_peripheral = peripherals.firstObject;
		_peripheral.delegate = self;
	}
}

- (void)centralManager:(CBCentralManager*)central
 didDiscoverPeripheral:(CBPeripheral*)peripheral
     advertisementData:(NSDictionary<NSString*,id>*)advertisementData
                  RSSI:(NSNumber*)RSSI
{
	if ([peripheral.identifier isEqual:_targetUuid]) {
		[central stopScan];
		_peripheral = peripheral;
		_peripheral.delegate = self;
		[central connectPeripheral:peripheral options:nil];
	}
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral
{
	NSLog(@"GNSS BLE: didConnectPeripheral: %@ — discovering services", peripheral.name);
	_transport->didConnectPeripheral();
	[peripheral discoverServices:nil];  // nil = discover ALL services for diagnosis
}

- (void)centralManager:(CBCentralManager*)central
didFailToConnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
	NSLog(@"GNSS BLE: didFailToConnect: %@", error);
	NSString* msg = error ? error.localizedDescription : @"Unknown error";
	_transport->didFailToConnect(QString::fromNSString(msg));
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
	NSLog(@"GNSS BLE: didDisconnect: %@", error);
	NSString* msg = error ? error.localizedDescription : @"";
	_transport->didDisconnectPeripheral(QString::fromNSString(msg));
	_rxCharacteristic = nil;
	_txCharacteristic = nil;
}


// ---- CBPeripheralDelegate ----

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverServices:(NSError*)error
{
	if (error) {
		NSLog(@"GNSS BLE: didDiscoverServices error: %@", error);
		_transport->didFailToConnect(QString::fromNSString(error.localizedDescription));
		return;
	}

	NSLog(@"GNSS BLE: discovered %lu services:", (unsigned long)peripheral.services.count);
	BOOL foundNus = NO;
	for (CBService* service in peripheral.services) {
		NSLog(@"GNSS BLE:   service: %@", service.UUID);
		if ([service.UUID isEqual:NUS_SERVICE_UUID]) {
			foundNus = YES;
			NSLog(@"GNSS BLE: found NUS service, discovering characteristics");
			[peripheral discoverCharacteristics:@[NUS_RX_CHAR_UUID, NUS_TX_CHAR_UUID]
			                         forService:service];
		}
	}
	if (!foundNus) {
		NSLog(@"GNSS BLE: NUS service NOT found on this device");
		_transport->didFailToConnect(
			QString::fromNSString(@"Device does not have Nordic UART Service (NUS). "
			                       "Check if your receiver firmware supports BLE UART."));
	}
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service
             error:(NSError*)error
{
	if (error) {
		_transport->didFailToConnect(QString::fromNSString(error.localizedDescription));
		return;
	}

	for (CBCharacteristic* ch in service.characteristics) {
		if ([ch.UUID isEqual:NUS_RX_CHAR_UUID]) {
			_rxCharacteristic = ch;
		} else if ([ch.UUID isEqual:NUS_TX_CHAR_UUID]) {
			_txCharacteristic = ch;
			// Subscribe to notifications from the receiver
			[peripheral setNotifyValue:YES forCharacteristic:ch];
		}
	}

	_transport->didDiscoverCharacteristics();
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
	BOOL enabled = characteristic.isNotifying;
	_transport->didUpdateNotificationState(enabled);

	// Report negotiated MTU once notifications are on
	NSInteger mtu = [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];
	_transport->didNegotiateMtu(static_cast<int>(mtu));
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
	_transport->didReceiveData(data);
}

- (void)peripheral:(CBPeripheral*)peripheral
didWriteValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
	if (error) {
		_transport->didFailToConnect(QString::fromNSString(error.localizedDescription));
	}
}

@end


// ============================================================================
// BleTransportCoreBluetooth — C++ implementation
// ============================================================================

namespace OpenOrienteering {


BleTransportCoreBluetooth::BleTransportCoreBluetooth(
    const QString& peripheralUuidString,
    const QString& deviceName,
    QObject* parent)
    : GnssTransport(parent)
    , m_peripheralUuid(peripheralUuidString)
    , m_deviceName(deviceName)
{
}

BleTransportCoreBluetooth::~BleTransportCoreBluetooth()
{
	disconnectFromDevice();
}


void BleTransportCoreBluetooth::connectToDevice()
{
	if (m_state == State::Connected || m_state == State::Connecting)
		return;

	setState(State::Connecting);

	if (!m_delegate) {
		m_delegate = [[BleCoreBtDelegate alloc]
			initWithTransport:this
			   peripheralUuid:m_peripheralUuid.toNSString()];
	}

	[m_delegate startConnection];
}


void BleTransportCoreBluetooth::disconnectFromDevice()
{
	if (m_delegate) {
		[m_delegate disconnect];
		m_delegate = nil;  // ARC releases
	}

	if (m_state != State::Disconnected)
		setState(State::Disconnected);
}


bool BleTransportCoreBluetooth::write(const QByteArray& data)
{
	if (m_state != State::Connected || !m_delegate)
		return false;

	return [m_delegate writeData:data];
}


GnssTransport::State BleTransportCoreBluetooth::state() const
{
	return m_state;
}


QString BleTransportCoreBluetooth::typeName() const
{
	return QStringLiteral("BLE");
}


QString BleTransportCoreBluetooth::deviceName() const
{
	return m_deviceName;
}


void BleTransportCoreBluetooth::setState(State newState)
{
	if (m_state != newState) {
		m_state = newState;
		emit stateChanged(m_state);
	}
}


// ---- Callbacks from ObjC delegate ----

void BleTransportCoreBluetooth::didUpdateState(int centralState)
{
	// CBManagerStatePoweredOn = 5
	if (centralState != 5 && m_state == State::Connecting) {
		emit errorOccurred(QStringLiteral("Bluetooth not available (state=%1)").arg(centralState));
	}
}

void BleTransportCoreBluetooth::didConnectPeripheral()
{
	// Connected to peripheral, but waiting for service/characteristic discovery
	// Stay in Connecting state until notifications are enabled
}

void BleTransportCoreBluetooth::didDisconnectPeripheral(const QString& error)
{
	setState(State::Disconnected);
	if (!error.isEmpty())
		emit errorOccurred(error);
}

void BleTransportCoreBluetooth::didFailToConnect(const QString& error)
{
	setState(State::Disconnected);
	emit errorOccurred(error);
}

void BleTransportCoreBluetooth::didDiscoverServices()
{
	// Services discovered, characteristics discovery in progress
}

void BleTransportCoreBluetooth::didDiscoverCharacteristics()
{
	// Characteristics discovered, waiting for notification subscription
}

void BleTransportCoreBluetooth::didUpdateNotificationState(bool enabled)
{
	if (enabled) {
		// Fully connected — notifications on TX characteristic active
		setState(State::Connected);
	}
}

void BleTransportCoreBluetooth::didReceiveData(const QByteArray& data)
{
	emit dataReceived(data);
}

void BleTransportCoreBluetooth::didWriteData(int bytesWritten)
{
	emit writeComplete(bytesWritten);
}

void BleTransportCoreBluetooth::didNegotiateMtu(int mtu)
{
	m_negotiatedMtu = mtu;
}


}  // namespace OpenOrienteering
