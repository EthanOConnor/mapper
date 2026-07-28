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


#include "android_usb_serial_transport.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QHash>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QStringList>


namespace OpenOrienteering {

namespace {

constexpr int android_usb_serial_disconnected = 0;
constexpr int android_usb_serial_connecting = 1;
constexpr int android_usb_serial_connected = 2;

QMutex& transportMutex()
{
	static QMutex mutex;
	return mutex;
}


QHash<qintptr, QPointer<AndroidUsbSerialTransport>>& transports()
{
	static QHash<qintptr, QPointer<AndroidUsbSerialTransport>> map;
	return map;
}


QPointer<AndroidUsbSerialTransport> transportForHandle(jlong handle)
{
	QMutexLocker lock(&transportMutex());
	return transports().value(static_cast<qintptr>(handle));
}


void registerTransport(AndroidUsbSerialTransport* transport)
{
	QMutexLocker lock(&transportMutex());
	transports().insert(reinterpret_cast<qintptr>(transport), transport);
}


void unregisterTransport(AndroidUsbSerialTransport* transport)
{
	QMutexLocker lock(&transportMutex());
	transports().remove(reinterpret_cast<qintptr>(transport));
}


void postToTransport(jlong handle, const QByteArray& data)
{
	auto transport = transportForHandle(handle);
	auto* app = QCoreApplication::instance();
	if (!transport || !app)
		return;

	QMetaObject::invokeMethod(app, [transport, data]() {
		if (transport)
			transport->handleDataReceived(data);
	}, Qt::QueuedConnection);
}


void postStateToTransport(jlong handle, int state)
{
	auto transport = transportForHandle(handle);
	auto* app = QCoreApplication::instance();
	if (!transport || !app)
		return;

	QMetaObject::invokeMethod(app, [transport, state]() {
		if (transport)
			transport->handleStateChanged(state);
	}, Qt::QueuedConnection);
}


void postErrorToTransport(jlong handle, const QString& message)
{
	auto transport = transportForHandle(handle);
	auto* app = QCoreApplication::instance();
	if (!transport || !app)
		return;

	QMetaObject::invokeMethod(app, [transport, message]() {
		if (transport)
			transport->handleError(message);
	}, Qt::QueuedConnection);
}


void postWriteCompleteToTransport(jlong handle, int bytes)
{
	auto transport = transportForHandle(handle);
	auto* app = QCoreApplication::instance();
	if (!transport || !app)
		return;

	QMetaObject::invokeMethod(app, [transport, bytes]() {
		if (transport)
			transport->handleWriteComplete(bytes);
	}, Qt::QueuedConnection);
}

}  // namespace


AndroidUsbSerialTransport::AndroidUsbSerialTransport(const QString& address,
                                                     const QString& deviceName,
                                                     QObject* parent)
    : GnssTransport(parent)
    , m_address(address)
    , m_deviceName(deviceName)
{
	registerTransport(this);
}


AndroidUsbSerialTransport::~AndroidUsbSerialTransport()
{
	unregisterTransport(this);
	disconnectFromDevice();
}


QList<AndroidUsbSerialDeviceInfo> AndroidUsbSerialTransport::availableDevices()
{
	QList<AndroidUsbSerialDeviceInfo> devices;
	auto list = QJniObject::callStaticObjectMethod(
	    "org/openorienteering/mapper/MapperUsbSerial",
	    "listDevices",
	    "()Ljava/lang/String;");
	if (!list.isValid())
		return devices;

	const auto lines = list.toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	for (const auto& line : lines)
	{
		const auto fields = line.split(QLatin1Char('\t'));
		if (fields.size() < 2)
			continue;

		AndroidUsbSerialDeviceInfo info;
		info.address = fields.at(0);
		info.name = fields.at(1);
		devices.append(info);
	}

	return devices;
}


void AndroidUsbSerialTransport::connectToDevice()
{
	if (m_state == State::Connected || m_state == State::Connecting)
		return;

	setState(State::Connecting);

	auto address = QJniObject::fromString(m_address);
	auto opened = QJniObject::callStaticMethod<jboolean>(
	    "org/openorienteering/mapper/MapperUsbSerial",
	    "open",
	    "(Ljava/lang/String;IJ)Z",
	    address.object<jstring>(),
	    static_cast<jint>(m_baudRate),
	    static_cast<jlong>(nativeHandle()));

	if (!opened)
	{
		emit errorOccurred(QStringLiteral("Could not open Android USB serial device"));
		setState(State::Disconnected);
	}
}


void AndroidUsbSerialTransport::disconnectFromDevice()
{
	QJniObject::callStaticMethod<void>(
	    "org/openorienteering/mapper/MapperUsbSerial",
	    "close",
	    "(J)V",
	    static_cast<jlong>(nativeHandle()));
	setState(State::Disconnected);
}


bool AndroidUsbSerialTransport::write(const QByteArray& data)
{
	if (m_state != State::Connected || data.isEmpty())
		return false;

	QJniEnvironment env;
	auto array = env->NewByteArray(data.size());
	if (!array)
		return false;

	env->SetByteArrayRegion(
	    array,
	    0,
	    data.size(),
	    reinterpret_cast<const jbyte*>(data.constData()));

	auto written = QJniObject::callStaticMethod<jboolean>(
	    "org/openorienteering/mapper/MapperUsbSerial",
	    "write",
	    "(J[B)Z",
	    static_cast<jlong>(nativeHandle()),
	    array);
	env->DeleteLocalRef(array);
	return written;
}


GnssTransport::State AndroidUsbSerialTransport::state() const
{
	return m_state;
}


QString AndroidUsbSerialTransport::typeName() const
{
	return QStringLiteral("USB serial");
}


QString AndroidUsbSerialTransport::deviceName() const
{
	return m_deviceName.isEmpty()
	    ? m_address + QLatin1String(" @ ") + QString::number(m_baudRate)
	    : m_deviceName;
}


void AndroidUsbSerialTransport::setBaudRate(qint32 baudRate)
{
	m_baudRate = baudRate;
}


void AndroidUsbSerialTransport::handleDataReceived(const QByteArray& data)
{
	emit dataReceived(data);
}


void AndroidUsbSerialTransport::handleStateChanged(int state)
{
	switch (state) {
	case android_usb_serial_connected:
		setState(State::Connected);
		break;
	case android_usb_serial_connecting:
		setState(State::Connecting);
		break;
	case android_usb_serial_disconnected:
	default:
		setState(State::Disconnected);
		break;
	}
}


void AndroidUsbSerialTransport::handleError(const QString& message)
{
	if (!message.isEmpty())
		emit errorOccurred(message);
}


void AndroidUsbSerialTransport::handleWriteComplete(int bytes)
{
	emit writeComplete(bytes);
}


qintptr AndroidUsbSerialTransport::nativeHandle() const
{
	return reinterpret_cast<qintptr>(this);
}


void AndroidUsbSerialTransport::setState(State newState)
{
	if (m_state == newState)
		return;

	m_state = newState;
	emit stateChanged(m_state);
}


}  // namespace OpenOrienteering


extern "C" JNIEXPORT void JNICALL
Java_org_openorienteering_mapper_MapperUsbSerial_nativeData(
    JNIEnv* env,
    jclass,
    jlong handle,
    jbyteArray bytes)
{
	if (!bytes)
		return;

	auto size = env->GetArrayLength(bytes);
	QByteArray data;
	data.resize(size);
	env->GetByteArrayRegion(
	    bytes,
	    0,
	    size,
	    reinterpret_cast<jbyte*>(data.data()));

	OpenOrienteering::postToTransport(handle, data);
}


extern "C" JNIEXPORT void JNICALL
Java_org_openorienteering_mapper_MapperUsbSerial_nativeStateChanged(
    JNIEnv*,
    jclass,
    jlong handle,
    jint state)
{
	OpenOrienteering::postStateToTransport(handle, static_cast<int>(state));
}


extern "C" JNIEXPORT void JNICALL
Java_org_openorienteering_mapper_MapperUsbSerial_nativeError(
    JNIEnv*,
    jclass,
    jlong handle,
    jstring message)
{
	OpenOrienteering::postErrorToTransport(
	    handle,
	    QJniObject(message).toString());
}


extern "C" JNIEXPORT void JNICALL
Java_org_openorienteering_mapper_MapperUsbSerial_nativeWriteComplete(
    JNIEnv*,
    jclass,
    jlong handle,
    jint bytes)
{
	OpenOrienteering::postWriteCompleteToTransport(handle, static_cast<int>(bytes));
}
