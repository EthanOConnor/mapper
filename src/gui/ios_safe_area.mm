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


#include "ios_safe_area.h"

#ifdef Q_OS_IOS
#import <UIKit/UIKit.h>
#endif


namespace OpenOrienteering {

namespace iOS {

QMargins safeAreaInsets()
{
#ifdef Q_OS_IOS
	UIWindow* window = nil;
	for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
	{
		if ([scene isKindOfClass:[UIWindowScene class]])
		{
			UIWindowScene* windowScene = (UIWindowScene*)scene;
			window = windowScene.windows.firstObject;
			break;
		}
	}
	if (!window)
		return {};

	UIEdgeInsets insets = window.safeAreaInsets;
	return QMargins(int(insets.left), int(insets.top),
	                int(insets.right), int(insets.bottom));
#else
	return {};
#endif
}

}  // namespace iOS

}  // namespace OpenOrienteering
