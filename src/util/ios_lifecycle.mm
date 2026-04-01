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


#include "ios_lifecycle.h"

#ifdef Q_OS_IOS
#import <UIKit/UIKit.h>
#endif


namespace OpenOrienteering {

namespace iOS {

unsigned long beginBackgroundTask()
{
#ifdef Q_OS_IOS
	UIBackgroundTaskIdentifier task_id = [UIApplication.sharedApplication
		beginBackgroundTaskWithExpirationHandler:^{
			// System is about to kill the background task; nothing we can do.
		}];
	return static_cast<unsigned long>(task_id);
#else
	return 0;
#endif
}


void endBackgroundTask(unsigned long identifier)
{
#ifdef Q_OS_IOS
	[UIApplication.sharedApplication endBackgroundTask:static_cast<UIBackgroundTaskIdentifier>(identifier)];
#else
	Q_UNUSED(identifier)
#endif
}


void lockOrientation()
{
	// TODO: iOS orientation locking requires subclassing the Qt view controller
	// and overriding supportedInterfaceOrientations.  Qt6 manages the root
	// UIViewController internally, so proper locking needs either a custom
	// QPA plugin or a runtime method-swizzle approach.  For now this is a
	// no-op; the app respects Info.plist UISupportedInterfaceOrientations.
}


void unlockOrientation()
{
	// TODO: See lockOrientation() — no-op until view controller subclassing
	// is implemented.
}


int displayRotation()
{
#ifdef Q_OS_IOS
	UIWindowScene* windowScene = nil;
	for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
	{
		if ([scene isKindOfClass:[UIWindowScene class]])
		{
			windowScene = (UIWindowScene*)scene;
			break;
		}
	}
	if (!windowScene)
		return 0;

	switch (windowScene.interfaceOrientation)
	{
	case UIInterfaceOrientationPortrait:
		return 0;
	case UIInterfaceOrientationLandscapeLeft:
		return 90;
	case UIInterfaceOrientationPortraitUpsideDown:
		return 180;
	case UIInterfaceOrientationLandscapeRight:
		return 270;
	default:
		return 0;
	}
#else
	return 0;
#endif
}

}  // namespace iOS

}  // namespace OpenOrienteering
