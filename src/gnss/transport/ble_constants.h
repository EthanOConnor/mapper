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


#ifndef OPENORIENTEERING_BLE_CONSTANTS_H
#define OPENORIENTEERING_BLE_CONSTANTS_H

#include <QBluetoothUuid>

namespace OpenOrienteering {

namespace BleGnss {


/// Nordic UART Service (NUS) UUIDs.
///
/// This is the de facto standard for BLE serial UART emulation, used by
/// virtually all BLE GNSS receivers and bridges (including ArduSimple
/// BT+BLE bridge, u-blox NINA modules, and many others).
///
/// The service provides bidirectional byte-stream communication:
///   - TX characteristic: receiver/bridge → phone (notifications)
///   - RX characteristic: phone → receiver/bridge (write)

/// NUS service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
inline const QBluetoothUuid kNusServiceUuid{
    QStringLiteral("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")};

/// NUS RX characteristic (phone writes to receiver): 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
inline const QBluetoothUuid kNusRxCharUuid{
    QStringLiteral("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")};

/// NUS TX characteristic (receiver notifies phone): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
inline const QBluetoothUuid kNusTxCharUuid{
    QStringLiteral("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")};


/// Desired MTU for BLE connections.
/// ArduSimple BLE bridge supports standard BLE MTU negotiation.
/// Higher MTU = fewer packets for RTCM correction forwarding.
constexpr int kDesiredMtu = 512;

/// Minimum acceptable MTU (default BLE 4.x MTU is 23, giving 20 bytes payload).
constexpr int kMinimumMtu = 23;


}  // namespace BleGnss

}  // namespace OpenOrienteering

#endif
