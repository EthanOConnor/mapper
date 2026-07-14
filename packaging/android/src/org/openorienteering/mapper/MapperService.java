/*
 *    Copyright 2019-2026 Kai Pastor
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

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;

import androidx.core.app.ServiceCompat;


/** Keeps live map editing and GPX recording active while Mapper is backgrounded. */
public class MapperService extends Service
{
	public static final String MESSAGE_EXTRA = "org.openorienteering.mapper.MESSAGE";

	private static final int NOTIFICATION_ID = 1;
	private static final String CHANNEL_ID = "live-mapping";

	@Override
	public int onStartCommand(Intent intent, int flags, int startId)
	{
		NotificationManager notificationManager = getSystemService(NotificationManager.class);
		notificationManager.createNotificationChannel(new NotificationChannel(
			CHANNEL_ID,
			getString(R.string.live_mapping_channel),
			NotificationManager.IMPORTANCE_LOW));

		Intent notificationIntent = new Intent(this, MapperActivity.class);
		PendingIntent pendingIntent = PendingIntent.getActivity(
			this,
			0,
			notificationIntent,
			PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
		Notification notification = new Notification.Builder(this, CHANNEL_ID)
			.setSmallIcon(android.R.drawable.ic_dialog_map)
			.setContentTitle(getString(R.string.app_name))
			.setContentText(intent.getStringExtra(MESSAGE_EXTRA))
			.setContentIntent(pendingIntent)
			.setOngoing(true)
			.build();

		ServiceCompat.startForeground(
			this,
			NOTIFICATION_ID,
			notification,
			ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION);
		return START_NOT_STICKY;
	}

	@Override
	public IBinder onBind(Intent intent)
	{
		return null;
	}
}
