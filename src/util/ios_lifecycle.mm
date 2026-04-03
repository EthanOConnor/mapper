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
#import <objc/runtime.h>
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



#ifdef Q_OS_IOS
namespace {

UIInterfaceOrientationMask g_locked_orientations = 0;
IMP g_original_supportedOrientations = nullptr;

UIInterfaceOrientationMask swizzled_supportedInterfaceOrientations(id self, SEL _cmd)
{
	if (g_locked_orientations)
		return g_locked_orientations;
	return reinterpret_cast<UIInterfaceOrientationMask(*)(id, SEL)>(g_original_supportedOrientations)(self, _cmd);
}

void ensureSwizzled()
{
	static bool done = false;
	if (done)
		return;
	done = true;

	Class vcClass = objc_getClass("QIOSViewController");
	if (!vcClass)
		return;  // Qt's view controller not found; do nothing

	SEL sel = @selector(supportedInterfaceOrientations);
	Method inherited = class_getInstanceMethod(vcClass, sel);
	g_original_supportedOrientations = method_getImplementation(inherited);

	// Add an override to QIOSViewController specifically, not to the
	// inherited UIViewController method. This scopes the swizzle to
	// Qt's root controller and leaves unrelated controllers untouched.
	class_addMethod(vcClass, sel,
	    reinterpret_cast<IMP>(swizzled_supportedInterfaceOrientations),
	    method_getTypeEncoding(inherited));
}

UIWindowScene* activeWindowScene()
{
	for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
	{
		if ([scene isKindOfClass:[UIWindowScene class]])
			return (UIWindowScene*)scene;
	}
	return nil;
}

void requestOrientationUpdate(UIWindowScene* windowScene, UIInterfaceOrientationMask mask)
{
	if (!windowScene)
		return;
	auto* prefs = [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
	[windowScene requestGeometryUpdateWithPreferences:prefs errorHandler:nil];
	for (UIWindow* w in windowScene.windows)
	{
		if (w.rootViewController)
			[w.rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
	}
}

}  // anonymous namespace
#endif


void lockOrientation()
{
#ifdef Q_OS_IOS
	ensureSwizzled();

	auto* windowScene = activeWindowScene();
	switch (windowScene.interfaceOrientation)
	{
	case UIInterfaceOrientationPortrait:
		g_locked_orientations = UIInterfaceOrientationMaskPortrait;
		break;
	case UIInterfaceOrientationLandscapeLeft:
		g_locked_orientations = UIInterfaceOrientationMaskLandscapeLeft;
		break;
	case UIInterfaceOrientationLandscapeRight:
		g_locked_orientations = UIInterfaceOrientationMaskLandscapeRight;
		break;
	default:
		g_locked_orientations = UIInterfaceOrientationMaskPortrait;
		break;
	}

	requestOrientationUpdate(windowScene, g_locked_orientations);
#endif
}


void unlockOrientation()
{
#ifdef Q_OS_IOS
	g_locked_orientations = 0;

	auto* windowScene = activeWindowScene();
	requestOrientationUpdate(windowScene, 0);
#endif
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
