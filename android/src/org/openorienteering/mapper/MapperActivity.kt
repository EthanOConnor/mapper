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

package org.openorienteering.mapper

import android.app.AlertDialog
import android.content.Intent
import android.content.pm.ActivityInfo
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.Surface
import org.qtproject.qt.android.bindings.QtActivity

/**
 * Companion class to Qt's standard Activity, providing static methods
 * callable from C++ via JNI for Android-specific functionality.
 *
 * The Qt6 deployment uses [QtActivity] as the actual activity class
 * (declared in AndroidManifest.xml). This class holds the static
 * methods that C++ code calls via QAndroidJniObject::callStaticMethod.
 *
 * The activity instance is captured in [onCreate] and used by all
 * static methods. The instance reference is the Qt-managed activity,
 * not an instance of this class.
 */
class MapperActivity : QtActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        instance = this
    }

    /** Handle re-launch with singleTask launch mode. */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
    }

    /**
     * Returns the data string from the launching intent and clears it.
     *
     * Called as an instance method from C++ via:
     *   activity.callObjectMethod<jstring>("takeIntentPath")
     */
    fun takeIntentPath(): String {
        val intent = getIntent() ?: return ""
        val action = intent.action
        if (action == Intent.ACTION_EDIT || action == Intent.ACTION_VIEW) {
            val result = intent.dataString ?: ""
            setIntent(null)
            return result
        }
        return ""
    }

    companion object {
        private var instance: MapperActivity? = null
        private var serviceStarted = false

        private var yesString = "Yes"
        private var noString = "No"
        private var gpsDisabledString = "GPS is disabled. Open settings?"

        /**
         * Receives translated UI strings from C++ for the GPS dialog.
         *
         * Called from C++ with JNI signature:
         *   (Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V
         */
        @JvmStatic
        fun setTranslatableStrings(yes: String, no: String, gpsDisabled: String) {
            yesString = yes
            noString = no
            gpsDisabledString = gpsDisabled
        }

        /**
         * Checks if GPS is enabled; shows a settings dialog if not.
         *
         * The dialog runs asynchronously on the UI thread.
         */
        @JvmStatic
        fun checkGPSEnabled() {
            val activity = instance ?: return
            val locationManager =
                activity.getSystemService(LOCATION_SERVICE) as? LocationManager ?: return
            if (!locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
                activity.runOnUiThread {
                    AlertDialog.Builder(activity)
                        .setMessage(gpsDisabledString)
                        .setPositiveButton(yesString) { _, _ ->
                            val intent = Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)
                            activity.startActivity(intent)
                        }
                        .setNegativeButton(noString, null)
                        .show()
                }
            }
        }

        /**
         * Locks the current screen orientation.
         *
         * Uses SCREEN_ORIENTATION_LOCKED (API 18+), which is always
         * available since our minSdk is 28.
         */
        @JvmStatic
        fun lockOrientation() {
            val activity = instance ?: return
            activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LOCKED
        }

        /** Unlocks the screen orientation. */
        @JvmStatic
        fun unlockOrientation() {
            val activity = instance ?: return
            activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }

        /**
         * Returns the display's current rotation as 0, 1, 2, or 3.
         *
         * Used by the compass to adjust azimuth for device orientation.
         */
        @JvmStatic
        fun getDisplayRotation(): Int {
            val activity = instance ?: return Surface.ROTATION_0
            return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                activity.display?.rotation ?: Surface.ROTATION_0
            } else {
                @Suppress("DEPRECATION")
                activity.windowManager.defaultDisplay.rotation
            }
        }

        /**
         * Starts the foreground service with a notification showing [filename].
         *
         * Uses startForegroundService() for API 26+ (always true since minSdk 28).
         */
        @JvmStatic
        fun startService(filename: String) {
            val activity = instance ?: return
            if (!serviceStarted) {
                serviceStarted = true
                val intent = Intent(activity, MapperService::class.java).apply {
                    putExtra("message", filename)
                }
                activity.startForegroundService(intent)
            }
        }

        /** Stops the foreground service. */
        @JvmStatic
        fun stopService() {
            val activity = instance ?: return
            if (serviceStarted) {
                val intent = Intent(activity, MapperService::class.java)
                activity.stopService(intent)
                serviceStarted = false
            }
        }
    }
}
