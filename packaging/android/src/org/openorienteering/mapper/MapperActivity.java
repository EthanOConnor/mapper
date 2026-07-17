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
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;

import java.lang.ref.WeakReference;
import java.util.WeakHashMap;

import org.qtproject.qt.android.bindings.QtActivity;


/** Android platform integration which is not provided by Qt. */
public class MapperActivity extends QtActivity
{
	private static MapperActivity instance;
	private static boolean serviceStarted;
	private static boolean optimizationRequestDone;
	private static final WeakHashMap<TextureView, WeakReference<Surface>> textureViewSurfaces =
		new WeakHashMap<>();
	private static final WeakHashMap<ViewGroup, WeakReference<SurfaceView>> nativeSurfaceViews =
		new WeakHashMap<>();

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
		{
			int flags = intent.getFlags()
				& (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
			if ((intent.getFlags() & Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) != 0
			    && flags != 0)
			{
				try
				{
					getContentResolver().takePersistableUriPermission(uri, flags);
				}
				catch (SecurityException ignored)
				{
					// Some providers advertise persistence but reject the request.
				}
			}
			return uri.toString();
		}
		return "";
	}

	/** Keep read/write access to a directory selected through Android's document picker. */
	public boolean persistDocumentTreeUri(String uriString)
	{
		if (uriString == null || uriString.isEmpty())
			return false;
		try
		{
			Uri uri = Uri.parse(uriString);
			getContentResolver().takePersistableUriPermission(
				uri,
				Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
			return true;
		}
		catch (RuntimeException ignored)
		{
			return false;
		}
	}

	/** Check a migrated document without modifying it. */
	public boolean canReadWriteDocument(String uriString)
	{
		if (uriString == null || uriString.isEmpty())
			return false;
		try (ParcelFileDescriptor descriptor =
			     getContentResolver().openFileDescriptor(Uri.parse(uriString), "rw"))
		{
			return descriptor != null;
		}
		catch (Exception ignored)
		{
			return false;
		}
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

	/**
	 * Return a public Android Surface for a Qt child QWindow.
	 * Qt does not create a native surface for a bare Vulkan QWindow, so install
	 * a SurfaceView in Mapper's render-only child window when needed.
	 */
	public static Surface nativeSurfaceForQtWindow(Object qtWindow)
	{
		if (!(qtWindow instanceof View))
			return null;
		View view = (View)qtWindow;
		Surface surface = findSurface(view);
		if (surface != null || !(view instanceof ViewGroup))
			return surface;

		ViewGroup group = (ViewGroup)view;
		WeakReference<SurfaceView> reference = nativeSurfaceViews.get(group);
		SurfaceView surfaceView = reference == null ? null : reference.get();
		if (surfaceView == null)
		{
			surfaceView = new SurfaceView(group.getContext());
			surfaceView.setClickable(false);
			surfaceView.setFocusable(false);
			nativeSurfaceViews.put(group, new WeakReference<>(surfaceView));
			group.addView(surfaceView, 0, new ViewGroup.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT,
				ViewGroup.LayoutParams.MATCH_PARENT));
		}
		surface = surfaceView.getHolder().getSurface();
		return surface != null && surface.isValid() ? surface : null;
	}

	private static Surface findSurface(View view)
	{
		if (view instanceof SurfaceView)
		{
			Surface surface = ((SurfaceView)view).getHolder().getSurface();
			return surface != null && surface.isValid() ? surface : null;
		}
		if (view instanceof TextureView)
		{
			TextureView textureView = (TextureView)view;
			WeakReference<Surface> reference = textureViewSurfaces.get(textureView);
			Surface surface = reference == null ? null : reference.get();
			if (surface != null && surface.isValid())
				return surface;
			if (!textureView.isAvailable() || textureView.getSurfaceTexture() == null)
				return null;
			surface = new Surface(textureView.getSurfaceTexture());
			textureViewSurfaces.put(textureView, new WeakReference<>(surface));
			return surface.isValid() ? surface : null;
		}
		if (view instanceof ViewGroup)
		{
			ViewGroup group = (ViewGroup)view;
			for (int index = 0; index < group.getChildCount(); ++index)
			{
				Surface surface = findSurface(group.getChildAt(index));
				if (surface != null)
					return surface;
			}
		}
		return null;
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
