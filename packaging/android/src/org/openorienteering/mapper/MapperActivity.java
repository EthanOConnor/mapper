/*
 *    Copyright 2013 Thomas Schöps
 *    Copyright 2014-2026 Thomas Schöps, Kai Pastor
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

import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.location.LocationManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.Display;

import org.qtproject.qt.android.bindings.QtActivity;


/** Android platform integration which is not provided by Qt. */
public class MapperActivity extends QtActivity
{
	private static MapperActivity instance;
	private static boolean serviceStarted;
	private static boolean optimizationRequestDone;

	private String yesString;
	private String noString;
	private String gpsDisabledString;

	@Override
	public void onCreate(Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);
		instance = this;
	}

	/** Preserve a document intent delivered to the single activity instance. */
	@Override
	public void onNewIntent(Intent intent)
	{
		super.onNewIntent(intent);
		setIntent(intent);
	}

	/** Return a pending document URI once. */
	public String takeIntentUri()
	{
		Intent intent = getIntent();
		if (intent == null)
			return "";

		String action = intent.getAction();
		Uri uri = intent.getData();
		setIntent(null);
		if ((Intent.ACTION_EDIT.equals(action) || Intent.ACTION_VIEW.equals(action)) && uri != null)
			return uri.toString();
		return "";
	}

	/** Offer the system battery-optimization exemption required by live tracking. */
	private void requestIgnoreBatteryOptimizations()
	{
		if (optimizationRequestDone)
			return;

		PowerManager powerManager = (PowerManager)getSystemService(POWER_SERVICE);
		if (powerManager != null && !powerManager.isIgnoringBatteryOptimizations(getPackageName()))
		{
			Intent intent = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
			intent.setData(Uri.parse("package:" + getPackageName()));
			startActivity(intent);
		}
		optimizationRequestDone = true;
	}

	/** Prompt the user to enable location services when the GPS provider is off. */
	public static void checkGPSEnabled()
	{
		LocationManager locationManager =
			(LocationManager)instance.getSystemService(LOCATION_SERVICE);
		if (locationManager == null
		    || locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER))
		{
			return;
		}

		instance.runOnUiThread(() -> new AlertDialog.Builder(instance)
			.setMessage(instance.gpsDisabledString)
			.setPositiveButton(instance.yesString, (dialog, which) ->
				instance.startActivity(new Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)))
			.setNegativeButton(instance.noString, null)
			.show());
	}

	public static void setTranslatableStrings(
		String yesString,
		String noString,
		String gpsDisabledString)
	{
		instance.yesString = yesString;
		instance.noString = noString;
		instance.gpsDisabledString = gpsDisabledString;
	}

	public static void lockOrientation()
	{
		instance.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LOCKED);
	}

	public static void unlockOrientation()
	{
		instance.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
	}

	public static int getDisplayRotation()
	{
		Display display = instance.getWindow().getDecorView().getDisplay();
		return display == null ? 0 : display.getRotation();
	}

	public static void startService(String message)
	{
		if (serviceStarted)
			return;

		serviceStarted = true;
		Intent intent = new Intent(instance, MapperService.class);
		intent.putExtra(MapperService.MESSAGE_EXTRA, message);
		instance.startForegroundService(intent);
		instance.requestIgnoreBatteryOptimizations();
	}

	public static void stopService()
	{
		if (!serviceStarted)
			return;

		instance.stopService(new Intent(instance, MapperService.class));
		serviceStarted = false;
	}
}
