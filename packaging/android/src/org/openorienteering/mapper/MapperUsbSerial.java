/*
 *    Copyright 2026 The OpenOrienteering developers
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

package org.openorienteering.mapper;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;

import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.driver.UsbSerialProber;
import com.hoho.android.usbserial.util.SerialInputOutputManager;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Android USB-host serial bridge used by AndroidUsbSerialTransport.
 *
 * It keeps Android permission and driver details on the Java side and exposes
 * a small handle-based API to the C++ GNSS transport layer. This is the Java
 * port of the Kotlin implementation used by the forward product line, without
 * the androidx dependency.
 */
public class MapperUsbSerial
{
	private static final String ACTION_USB_PERMISSION = "org.openorienteering.mapper.USB_PERMISSION";
	private static final int READ_TIMEOUT_MS = 1000;

	private static final int STATE_DISCONNECTED = 0;
	@SuppressWarnings("unused")
	private static final int STATE_CONNECTING = 1;
	private static final int STATE_CONNECTED = 2;

	private static class PortMatch
	{
		final UsbSerialDriver driver;
		final int portIndex;

		PortMatch(UsbSerialDriver driver, int portIndex)
		{
			this.driver = driver;
			this.portIndex = portIndex;
		}
	}

	private static class OpenRequest
	{
		final String address;
		final int baudRate;
		final long nativeHandle;

		OpenRequest(String address, int baudRate, long nativeHandle)
		{
			this.address = address;
			this.baudRate = baudRate;
			this.nativeHandle = nativeHandle;
		}
	}

	private static class OpenConnection
	{
		final UsbSerialPort port;
		final UsbDeviceConnection deviceConnection;
		final SerialInputOutputManager ioManager;

		OpenConnection(UsbSerialPort port, UsbDeviceConnection deviceConnection,
		               SerialInputOutputManager ioManager)
		{
			this.port = port;
			this.deviceConnection = deviceConnection;
			this.ioManager = ioManager;
		}
	}

	private static final Map<String, OpenRequest> pendingRequests = new HashMap<>();
	private static final Map<Long, OpenConnection> openConnections = new HashMap<>();
	private static boolean receiverRegistered = false;

	private static final BroadcastReceiver permissionReceiver = new BroadcastReceiver()
	{
		@Override
		public void onReceive(Context context, Intent intent)
		{
			if (!ACTION_USB_PERMISSION.equals(intent.getAction()))
				return;

			UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
			if (device == null)
				return;

			List<OpenRequest> requests = new ArrayList<>();
			synchronized (MapperUsbSerial.class)
			{
				String prefix = device.getDeviceName() + "|";
				Iterator<Map.Entry<String, OpenRequest>> it = pendingRequests.entrySet().iterator();
				while (it.hasNext())
				{
					Map.Entry<String, OpenRequest> entry = it.next();
					if (entry.getKey().startsWith(prefix))
					{
						requests.add(entry.getValue());
						it.remove();
					}
				}
			}

			if (!intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
			{
				for (OpenRequest request : requests)
				{
					nativeError(request.nativeHandle, "USB permission denied");
					nativeStateChanged(request.nativeHandle, STATE_DISCONNECTED);
				}
				return;
			}

			UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
			if (usbManager == null)
			{
				for (OpenRequest request : requests)
				{
					nativeError(request.nativeHandle, "USB manager unavailable");
					nativeStateChanged(request.nativeHandle, STATE_DISCONNECTED);
				}
				return;
			}

			for (OpenRequest request : requests)
			{
				PortMatch match = findPort(usbManager, request.address);
				if (match == null)
				{
					nativeError(request.nativeHandle, "USB serial device is no longer available");
					nativeStateChanged(request.nativeHandle, STATE_DISCONNECTED);
				}
				else
				{
					openWithPermission(usbManager, match, request);
				}
			}
		}
	};

	/**
	 * Lists the available USB serial ports.
	 *
	 * One line per port: address, then a tab, then a display name.
	 */
	public static String listDevices()
	{
		Context context = appContext();
		if (context == null)
			return "";
		UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
		if (usbManager == null)
			return "";

		StringBuilder result = new StringBuilder();
		for (UsbSerialDriver driver : UsbSerialProber.getDefaultProber().findAllDrivers(usbManager))
		{
			int portCount = driver.getPorts().size();
			for (int index = 0; index < portCount; ++index)
			{
				if (result.length() > 0)
					result.append('\n');
				result.append(sanitize(addressFor(driver.getDevice(), index)));
				result.append('\t');
				result.append(sanitize(displayNameFor(driver, index)));
			}
		}
		return result.toString();
	}

	/** Opens a port, requesting USB permission from the user first if needed. */
	public static boolean open(String address, int baudRate, long nativeHandle)
	{
		Context context = appContext();
		if (context == null)
			return false;
		UsbManager usbManager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
		if (usbManager == null)
			return false;

		PortMatch match = findPort(usbManager, address);
		if (match == null)
		{
			nativeError(nativeHandle, "USB serial device not found");
			nativeStateChanged(nativeHandle, STATE_DISCONNECTED);
			return true;
		}

		close(nativeHandle);

		OpenRequest request = new OpenRequest(address, baudRate, nativeHandle);
		UsbDevice device = match.driver.getDevice();
		if (!usbManager.hasPermission(device))
		{
			synchronized (MapperUsbSerial.class)
			{
				pendingRequests.put(address, request);
			}
			requestPermission(context, usbManager, device);
			return true;
		}

		openWithPermission(usbManager, match, request);
		return true;
	}

	/** Closes the port for the given native handle, if open. */
	public static void close(long nativeHandle)
	{
		OpenConnection connection;
		synchronized (MapperUsbSerial.class)
		{
			Iterator<Map.Entry<String, OpenRequest>> it = pendingRequests.entrySet().iterator();
			while (it.hasNext())
			{
				if (it.next().getValue().nativeHandle == nativeHandle)
					it.remove();
			}
			connection = openConnections.remove(nativeHandle);
		}
		if (connection == null)
			return;
		try { connection.ioManager.stop(); } catch (Exception ignored) {}
		try { connection.port.close(); } catch (Exception ignored) {}
		try { connection.deviceConnection.close(); } catch (Exception ignored) {}
	}

	/** Writes bytes (typically RTCM corrections) to the open port. */
	public static boolean write(long nativeHandle, byte[] data)
	{
		OpenConnection connection;
		synchronized (MapperUsbSerial.class)
		{
			connection = openConnections.get(nativeHandle);
		}
		if (connection == null)
			return false;
		try
		{
			connection.ioManager.writeAsync(data);
			nativeWriteComplete(nativeHandle, data.length);
			return true;
		}
		catch (Exception error)
		{
			nativeError(nativeHandle,
			            error.getMessage() != null ? error.getMessage() : "USB serial write failed");
			return false;
		}
	}

	private static void openWithPermission(UsbManager usbManager, PortMatch match,
	                                       final OpenRequest request)
	{
		UsbDeviceConnection deviceConnection = usbManager.openDevice(match.driver.getDevice());
		if (deviceConnection == null)
		{
			nativeError(request.nativeHandle, "Could not open USB device");
			nativeStateChanged(request.nativeHandle, STATE_DISCONNECTED);
			return;
		}

		UsbSerialPort port = match.driver.getPorts().get(match.portIndex);
		try
		{
			port.open(deviceConnection);
			port.setParameters(request.baudRate, 8,
			                   UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE);
			try { port.setDTR(true); } catch (Exception ignored) {}
			try { port.setRTS(true); } catch (Exception ignored) {}

			SerialInputOutputManager ioManager = new SerialInputOutputManager(
			    port,
			    new SerialInputOutputManager.Listener()
			    {
				@Override
				public void onNewData(byte[] data)
				{
					nativeData(request.nativeHandle, data);
				}

				@Override
				public void onRunError(Exception error)
				{
					nativeError(request.nativeHandle,
					            error.getMessage() != null
					                ? error.getMessage() : "USB serial connection lost");
					close(request.nativeHandle);
					nativeStateChanged(request.nativeHandle, STATE_DISCONNECTED);
				}
			    });

			synchronized (MapperUsbSerial.class)
			{
				openConnections.put(request.nativeHandle,
				                    new OpenConnection(port, deviceConnection, ioManager));
			}
			ioManager.setReadTimeout(READ_TIMEOUT_MS);
			ioManager.start();
			nativeStateChanged(request.nativeHandle, STATE_CONNECTED);
		}
		catch (Exception error)
		{
			try { port.close(); } catch (Exception ignored) {}
			try { deviceConnection.close(); } catch (Exception ignored) {}
			nativeError(request.nativeHandle,
			            error.getMessage() != null
			                ? error.getMessage() : "Could not open USB serial port");
			nativeStateChanged(request.nativeHandle, STATE_DISCONNECTED);
		}
	}

	private static void requestPermission(Context context, UsbManager usbManager, UsbDevice device)
	{
		ensurePermissionReceiver(context);
		int flags = PendingIntent.FLAG_UPDATE_CURRENT;
		if (Build.VERSION.SDK_INT >= 31)  // Build.VERSION_CODES.S
			flags |= PendingIntent.FLAG_MUTABLE;
		Intent intent = new Intent(ACTION_USB_PERMISSION).setPackage(context.getPackageName());
		PendingIntent pendingIntent = PendingIntent.getBroadcast(
		    context, device.getDeviceName().hashCode(), intent, flags);
		usbManager.requestPermission(device, pendingIntent);
	}

	private static synchronized void ensurePermissionReceiver(Context context)
	{
		if (receiverRegistered)
			return;

		IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
		if (Build.VERSION.SDK_INT >= 33)  // Build.VERSION_CODES.TIRAMISU
			context.registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
		else
			context.registerReceiver(permissionReceiver, filter);
		receiverRegistered = true;
	}

	private static PortMatch findPort(UsbManager usbManager, String address)
	{
		for (UsbSerialDriver driver : UsbSerialProber.getDefaultProber().findAllDrivers(usbManager))
		{
			int portCount = driver.getPorts().size();
			for (int index = 0; index < portCount; ++index)
			{
				if (addressFor(driver.getDevice(), index).equals(address))
					return new PortMatch(driver, index);
			}
		}
		return null;
	}

	private static String addressFor(UsbDevice device, int portIndex)
	{
		return device.getDeviceName() + "|" + portIndex;
	}

	private static String displayNameFor(UsbSerialDriver driver, int portIndex)
	{
		UsbDevice device = driver.getDevice();
		String product = null;
		if (Build.VERSION.SDK_INT >= 21)
		{
			try { product = device.getProductName(); } catch (Exception ignored) {}
		}
		if (product == null)
		{
			product = driver.getClass().getSimpleName();
			if (product.endsWith("SerialDriver"))
				product = product.substring(0, product.length() - "SerialDriver".length());
		}
		String manufacturer = null;
		if (Build.VERSION.SDK_INT >= 21)
		{
			try { manufacturer = device.getManufacturerName(); } catch (Exception ignored) {}
		}
		String vidPid = String.format(Locale.US, "%04X:%04X",
		                              device.getVendorId(), device.getProductId());
		String portLabel = driver.getPorts().size() > 1
		    ? " port " + (portIndex + 1) : "";

		StringBuilder name = new StringBuilder();
		if (manufacturer != null && !manufacturer.isEmpty())
			name.append(manufacturer).append(' ');
		name.append(product).append(portLabel).append(' ').append(vidPid);
		return name.toString();
	}

	private static String sanitize(String value)
	{
		return value.replace('\t', ' ').replace('\n', ' ').trim();
	}

	private static Context appContext()
	{
		MapperActivity activity = MapperActivity.currentActivity();
		return activity != null ? activity.getApplicationContext() : null;
	}

	public static native void nativeData(long nativeHandle, byte[] data);
	public static native void nativeStateChanged(long nativeHandle, int state);
	public static native void nativeError(long nativeHandle, String message);
	public static native void nativeWriteComplete(long nativeHandle, int bytes);
}
