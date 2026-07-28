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

#include "ios_ble_nus_transport.h"

#import <CoreBluetooth/CoreBluetooth.h>

#include <QByteArray>
#include <QString>


// ---- NUS UUIDs (ARC is active, static strong refs are safe) ----

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


// Max connect attempts during initial connection (not reconnect)
static const int kMaxConnectAttempts = 3;

// Timeout per connect attempt (ArduSimple can take 3-4s)
static const NSTimeInterval kConnectTimeout = 5.0;


// ============================================================================
// Internal connection phase — gates ObjC callbacks
// ============================================================================

typedef NS_ENUM(NSInteger, NusConnectionPhase) {
	PhaseIdle,
	PhaseConnecting,
	PhaseDiscoveringServices,
	PhaseDiscoveringCharacteristics,
	PhaseSubscribing,
	PhaseReady
};


// ============================================================================
// IosBleNusDelegate — single-purpose: own and drive one NUS BLE connection
// ============================================================================

@interface IosBleNusDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
{
	OpenOrienteering::IosBleNusTransport* _transport;
	CBCentralManager* _centralManager;
	CBPeripheral* _peripheral;
	CBCharacteristic* _rxCharacteristic;
	CBCharacteristic* _txCharacteristic;
	NSUUID* _peripheralUuid;
	NSTimer* _connectTimeoutTimer;
	NusConnectionPhase _phase;
	int _connectAttempt;
	BOOL _isReconnect;  // true when auto-reconnecting (no timeout, CB waits)
}
- (instancetype)initWithTransport:(OpenOrienteering::IosBleNusTransport*)transport
                   centralManager:(CBCentralManager*)manager
                       peripheral:(CBPeripheral*)peripheral
                   peripheralUuid:(NSUUID*)uuid;
- (void)startConnection;
- (void)startReconnect;
- (void)disconnectAndTeardown;
- (void)reconnectAfterPowerOn;
- (BOOL)writeData:(const QByteArray&)data;
@end


// Guard: bail from callback if C++ transport is already torn down
#define GUARD_TRANSPORT() do { if (!_transport) return; } while(0)


@implementation IosBleNusDelegate

- (instancetype)initWithTransport:(OpenOrienteering::IosBleNusTransport*)transport
                   centralManager:(CBCentralManager*)manager
                       peripheral:(CBPeripheral*)peripheral
                   peripheralUuid:(NSUUID*)uuid
{
	self = [super init];
	if (self) {
		_transport = transport;
		_centralManager = manager;
		_peripheral = peripheral;
		_peripheralUuid = uuid;
		_phase = PhaseIdle;
		_connectAttempt = 0;
		_isReconnect = NO;

		// Take over as delegate for both manager and peripheral
		_centralManager.delegate = self;
		if (_peripheral)
			_peripheral.delegate = self;
	}
	return self;
}


// ---- Connection control ----

- (void)startConnection
{
	if (!_peripheral || !_centralManager)
		return;

	_isReconnect = NO;
	_connectAttempt = 1;
	_phase = PhaseConnecting;

	NSLog(@"GNSS NUS: connecting to %@ (attempt %d/%d)",
	      _peripheral.name ?: _peripheralUuid.UUIDString,
	      _connectAttempt, kMaxConnectAttempts);

	_peripheral.delegate = self;
	[_centralManager connectPeripheral:_peripheral options:nil];

	// CoreBluetooth's connectPeripheral has no built-in timeout
	_connectTimeoutTimer = [NSTimer scheduledTimerWithTimeInterval:kConnectTimeout
	                                                       target:self
	                                                     selector:@selector(connectTimeout:)
	                                                     userInfo:nil
	                                                      repeats:NO];
}

- (void)startReconnect
{
	_isReconnect = YES;
	_connectAttempt = 0;
	_phase = PhaseConnecting;

	if (_peripheral) {
		NSLog(@"GNSS NUS: auto-reconnecting to %@", _peripheral.name ?: _peripheralUuid.UUIDString);
		_peripheral.delegate = self;
		[_centralManager connectPeripheral:_peripheral options:nil];
		// No timeout — CoreBluetooth queues the connect and succeeds when device is in range
	} else {
		// Peripheral was invalidated (e.g., BT power cycle) — try to retrieve
		[self reconnectAfterPowerOn];
	}
}

- (void)reconnectAfterPowerOn
{
	GUARD_TRANSPORT();
	if (!_peripheralUuid) {
		_transport->didFailToConnect(
			QString::fromNSString(@"No peripheral UUID for reconnect"));
		return;
	}

	NSArray<CBPeripheral*>* known = [_centralManager
		retrievePeripheralsWithIdentifiers:@[_peripheralUuid]];

	if (known.count > 0) {
		NSLog(@"GNSS NUS: retrieved peripheral by UUID after power-on, reconnecting");
		_peripheral = known.firstObject;
		_peripheral.delegate = self;
		[_centralManager connectPeripheral:_peripheral options:nil];
	} else {
		NSLog(@"GNSS NUS: peripheral not found after power-on, will retry on next connectToDevice");
		_transport->didFailToConnect(
			QString::fromNSString(@"Device not found after Bluetooth restart"));
	}
}

- (void)connectTimeout:(NSTimer*)timer
{
	_connectTimeoutTimer = nil;
	GUARD_TRANSPORT();

	if (_phase == PhaseReady)
		return;  // Connected while timer was firing

	NSLog(@"GNSS NUS: connect timeout (attempt %d/%d)", _connectAttempt, kMaxConnectAttempts);

	// Cancel the pending connection
	if (_peripheral)
		[_centralManager cancelPeripheralConnection:_peripheral];

	_connectAttempt++;
	if (_connectAttempt > kMaxConnectAttempts) {
		NSLog(@"GNSS NUS: all %d connect attempts exhausted", kMaxConnectAttempts);
		_phase = PhaseIdle;
		_transport->didFailToConnect(
			QString::fromNSString(@"Connection timed out after multiple attempts"));
		return;
	}

	// Retry
	NSLog(@"GNSS NUS: retrying (attempt %d/%d)", _connectAttempt, kMaxConnectAttempts);
	_phase = PhaseConnecting;
	if (_peripheral) {
		_peripheral.delegate = self;
		[_centralManager connectPeripheral:_peripheral options:nil];
		_connectTimeoutTimer = [NSTimer scheduledTimerWithTimeInterval:kConnectTimeout
		                                                       target:self
		                                                     selector:@selector(connectTimeout:)
		                                                     userInfo:nil
		                                                      repeats:NO];
	} else {
		_transport->didFailToConnect(
			QString::fromNSString(@"Peripheral lost during retry"));
	}
}

- (void)disconnectAndTeardown
{
	_transport = nil;
	_phase = PhaseIdle;
	[_connectTimeoutTimer invalidate];
	_connectTimeoutTimer = nil;

	if (_peripheral) {
		_peripheral.delegate = nil;
		if (_txCharacteristic && _txCharacteristic.isNotifying) {
			[_peripheral setNotifyValue:NO forCharacteristic:_txCharacteristic];
		}
		[_centralManager cancelPeripheralConnection:_peripheral];
		_peripheral = nil;
	}
	_rxCharacteristic = nil;
	_txCharacteristic = nil;
	_centralManager.delegate = nil;
}


// ---- Write ----

- (BOOL)writeData:(const QByteArray&)data
{
	if (!_transport || !_rxCharacteristic || !_peripheral || _phase != PhaseReady)
		return NO;

	CBCharacteristicWriteType writeType =
		(_rxCharacteristic.properties & CBCharacteristicPropertyWriteWithoutResponse)
			? CBCharacteristicWriteWithoutResponse
			: CBCharacteristicWriteWithResponse;

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


// ============================================================================
// CBCentralManagerDelegate
// ============================================================================

- (void)centralManagerDidUpdateState:(CBCentralManager*)central
{
	NSLog(@"GNSS NUS: centralManagerDidUpdateState: %ld", (long)central.state);
	GUARD_TRANSPORT();

	if (central.state == CBManagerStatePoweredOff) {
		// CoreBluetooth invalidates all peripheral objects on power-off
		[_connectTimeoutTimer invalidate];
		_connectTimeoutTimer = nil;
		_rxCharacteristic = nil;
		_txCharacteristic = nil;
		_peripheral = nil;
		_phase = PhaseIdle;
		_transport->didUpdateState(static_cast<int>(central.state));
	} else if (central.state == CBManagerStatePoweredOn) {
		_transport->didUpdateState(static_cast<int>(central.state));
	}
}

- (void)centralManager:(CBCentralManager*)central
         willRestoreState:(NSDictionary<NSString*,id>*)dict
{
	// State restoration after app relaunch by OS
	NSArray<CBPeripheral*>* peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey];
	if (peripherals.count > 0) {
		NSLog(@"GNSS NUS: state restoration — recovered peripheral");
		_peripheral = peripherals.firstObject;
		_peripheral.delegate = self;
	}
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral
{
	GUARD_TRANSPORT();
	NSLog(@"GNSS NUS: didConnectPeripheral: %@", peripheral.name);

	[_connectTimeoutTimer invalidate];
	_connectTimeoutTimer = nil;

	_transport->didConnectPeripheral();

	// Discover NUS service specifically (not nil)
	_phase = PhaseDiscoveringServices;
	[peripheral discoverServices:@[nusServiceUuid()]];
}

- (void)centralManager:(CBCentralManager*)central
didFailToConnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
	GUARD_TRANSPORT();
	NSLog(@"GNSS NUS: didFailToConnect: %@", error);

	// Cancel any pending timeout — this callback supersedes it
	[_connectTimeoutTimer invalidate];
	_connectTimeoutTimer = nil;

	if (!_isReconnect) {
		_connectAttempt++;
		if (_connectAttempt <= kMaxConnectAttempts) {
			NSLog(@"GNSS NUS: retrying after didFailToConnect (attempt %d/%d)",
			      _connectAttempt, kMaxConnectAttempts);
			_phase = PhaseConnecting;
			[_centralManager connectPeripheral:_peripheral options:nil];
			_connectTimeoutTimer = [NSTimer scheduledTimerWithTimeInterval:kConnectTimeout
			                                                       target:self
			                                                     selector:@selector(connectTimeout:)
			                                                     userInfo:nil
			                                                      repeats:NO];
			return;
		}
	}

	_phase = PhaseIdle;
	_transport->didFailToConnect(
		QString::fromNSString(error ? error.localizedDescription : @"Connection failed"));
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral
                 error:(NSError*)error
{
	GUARD_TRANSPORT();
	NSLog(@"GNSS NUS: didDisconnect: %@", error);

	_rxCharacteristic = nil;
	_txCharacteristic = nil;

	_transport->didDisconnectPeripheral(
		QString::fromNSString(error ? error.localizedDescription : @""));
}


// ============================================================================
// CBPeripheralDelegate
// ============================================================================

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverServices:(NSError*)error
{
	GUARD_TRANSPORT();
	if (_phase != PhaseDiscoveringServices)
		return;

	if (error) {
		NSLog(@"GNSS NUS: service discovery error: %@", error);
		_phase = PhaseIdle;
		_transport->didFailToConnect(QString::fromNSString(error.localizedDescription));
		return;
	}

	for (CBService* service in peripheral.services) {
		if ([service.UUID isEqual:nusServiceUuid()]) {
			NSLog(@"GNSS NUS: found NUS service — discovering characteristics");
			_phase = PhaseDiscoveringCharacteristics;
			[peripheral discoverCharacteristics:@[nusRxCharUuid(), nusTxCharUuid()]
			                         forService:service];
			_transport->didDiscoverNusService();
			return;
		}
	}

	NSLog(@"GNSS NUS: NUS service not found on device");
	_phase = PhaseIdle;
	_transport->didFailToConnect(
		QString::fromNSString(@"Device does not have Nordic UART Service (NUS)"));
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service
             error:(NSError*)error
{
	GUARD_TRANSPORT();
	if (_phase != PhaseDiscoveringCharacteristics)
		return;

	if (error) {
		NSLog(@"GNSS NUS: characteristic discovery error: %@", error);
		_phase = PhaseIdle;
		_transport->didFailToConnect(QString::fromNSString(error.localizedDescription));
		return;
	}

	BOOL rxFound = NO, txFound = NO;
	for (CBCharacteristic* ch in service.characteristics) {
		if ([ch.UUID isEqual:nusRxCharUuid()]) {
			_rxCharacteristic = ch;
			rxFound = YES;
			NSLog(@"GNSS NUS: found RX characteristic");
		} else if ([ch.UUID isEqual:nusTxCharUuid()]) {
			_txCharacteristic = ch;
			txFound = YES;
			NSLog(@"GNSS NUS: found TX characteristic — subscribing");
			_phase = PhaseSubscribing;
			[peripheral setNotifyValue:YES forCharacteristic:ch];
		}
	}

	_transport->didDiscoverNusCharacteristics(rxFound, txFound);

	if (!rxFound || !txFound) {
		NSLog(@"GNSS NUS: missing NUS characteristic(s) (rx=%d tx=%d)", rxFound, txFound);
		if (!txFound) {
			// Can't proceed without TX notifications
			_phase = PhaseIdle;
			_transport->didFailToConnect(
				QString::fromNSString(@"NUS TX characteristic not found"));
		}
	}
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
	GUARD_TRANSPORT();
	if (_phase != PhaseSubscribing)
		return;

	if (error) {
		NSLog(@"GNSS NUS: notification subscribe error: %@", error);
		_phase = PhaseIdle;
		_transport->didFailToConnect(QString::fromNSString(error.localizedDescription));
		return;
	}

	if (characteristic.isNotifying) {
		NSInteger mtu = [peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];
		NSLog(@"GNSS NUS: ready! MTU payload=%ld", (long)mtu);
		_phase = PhaseReady;
		_transport->didSubscribeToTx(static_cast<int>(mtu));
	}
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic
             error:(NSError*)error
{
	GUARD_TRANSPORT();
	if (error || !characteristic.value || _phase != PhaseReady)
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
		NSLog(@"GNSS NUS: write error: %@", error);
	}
}

@end


// ============================================================================
// IosBleNusTransport — C++ implementation
// ============================================================================

// CBManagerState values forwarded as int to C++ callbacks
static constexpr int kCBManagerStatePoweredOff = 4;
static constexpr int kCBManagerStatePoweredOn  = 5;

namespace OpenOrienteering {


IosBleNusTransport::IosBleNusTransport(
    CBCentralManager* manager,
    CBPeripheral* peripheral,
    const QString& peripheralUuid,
    const QString& deviceName,
    QObject* parent)
    : GnssTransport(parent)
    , m_peripheralUuid(peripheralUuid)
    , m_deviceName(deviceName)
{
	NSUUID* uuid = [[NSUUID alloc] initWithUUIDString:peripheralUuid.toNSString()];
	m_delegate = [[IosBleNusDelegate alloc] initWithTransport:this
	                                           centralManager:manager
	                                               peripheral:peripheral
	                                           peripheralUuid:uuid];
}

IosBleNusTransport::~IosBleNusTransport()
{
	if (m_delegate)
		[m_delegate disconnectAndTeardown];
}


void IosBleNusTransport::connectToDevice()
{
	if (m_state == State::Connected || m_state == State::Connecting)
		return;

	m_intentionalDisconnect = false;

	if (m_state == State::Reconnecting) {
		// Already auto-reconnecting — let it continue
		return;
	}

	setState(State::Connecting);

	if (m_delegate)
		[m_delegate startConnection];
}


void IosBleNusTransport::disconnectFromDevice()
{
	m_intentionalDisconnect = true;

	if (m_delegate)
		[m_delegate disconnectAndTeardown];

	// After intentional disconnect the session usually replaces us.
	// Nil the delegate — destructor handles cleanup.
	m_delegate = nullptr;

	if (m_state != State::Disconnected)
		setState(State::Disconnected);
}


bool IosBleNusTransport::write(const QByteArray& data)
{
	if (m_state != State::Connected || !m_delegate)
		return false;

	return [m_delegate writeData:data];
}


GnssTransport::State IosBleNusTransport::state() const
{
	return m_state;
}


QString IosBleNusTransport::typeName() const
{
	return QStringLiteral("BLE");
}


QString IosBleNusTransport::deviceName() const
{
	return m_deviceName;
}


void IosBleNusTransport::setState(State newState)
{
	if (m_state != newState) {
		m_state = newState;
		emit stateChanged(m_state);
	}
}


void IosBleNusTransport::attemptReconnect()
{
	if (m_intentionalDisconnect)
		return;

	setState(State::Reconnecting);

	if (m_delegate)
		[m_delegate startReconnect];
}


// ---- Callbacks from ObjC delegate ----

void IosBleNusTransport::didUpdateState(int centralState)
{
	if (centralState == kCBManagerStatePoweredOff) {
		// Bluetooth powered off — peripheral is invalidated
		if (m_state == State::Connected || m_state == State::Connecting) {
			// Auto-reconnect when BT comes back
			m_wasConnected = (m_state == State::Connected);
			attemptReconnect();
		}
	} else if (centralState == kCBManagerStatePoweredOn) {
		// Bluetooth powered on
		if (m_state == State::Reconnecting && m_delegate) {
			[m_delegate reconnectAfterPowerOn];
		}
	}
}

void IosBleNusTransport::didConnectPeripheral()
{
	// Physical connection established — service discovery starting
	// Stay in Connecting (or Reconnecting) until fully ready
}

void IosBleNusTransport::didDisconnectPeripheral(const QString& error)
{
	if (m_intentionalDisconnect) {
		setState(State::Disconnected);
		return;
	}

	if (m_wasConnected || m_state == State::Connected) {
		// Was previously connected — auto-reconnect
		m_wasConnected = true;
		attemptReconnect();
	} else {
		// Never successfully connected — report failure
		setState(State::Disconnected);
		if (!error.isEmpty())
			emit errorOccurred(error);
	}
}

void IosBleNusTransport::didFailToConnect(const QString& error)
{
	if (m_state == State::Reconnecting) {
		// Reconnect failed — stay in Reconnecting, session timer will retry
		emit errorOccurred(error);
		return;
	}

	setState(State::Disconnected);
	emit errorOccurred(error);
}

void IosBleNusTransport::didDiscoverNusService()
{
	// Service found, characteristic discovery in progress
}

void IosBleNusTransport::didDiscoverNusCharacteristics(bool rxFound, bool txFound)
{
	Q_UNUSED(rxFound)
	Q_UNUSED(txFound)
	// Characteristics found (or not), subscription in progress
}

void IosBleNusTransport::didSubscribeToTx(int mtu)
{
	m_negotiatedMtu = mtu;
	m_wasConnected = true;
	m_intentionalDisconnect = false;

	// Fully connected — NUS TX notifications active, ready for data
	setState(State::Connected);
}

void IosBleNusTransport::didReceiveData(const QByteArray& data)
{
	emit dataReceived(data);
}

void IosBleNusTransport::didWriteData(int bytesWritten)
{
	emit writeComplete(bytesWritten);
}


}  // namespace OpenOrienteering
