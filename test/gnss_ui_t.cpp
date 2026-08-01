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

#include "gnss_ui_t.h"

#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

#include "gnss/gnss_state.h"
#include "gnss/ui/gnss_detail_panel.h"
#include "gnss/ui/gnss_settings_page.h"
#include "gnss/ui/gnss_status_overlay.h"

using namespace OpenOrienteering;


void GnssUiTest::overlayDistinguishesLinkFromPosition()
{
	GnssStatusOverlay overlay;
	overlay.resize(overlay.sizeHint());
	GnssState state;
	state.transportState = GnssTransportState::Connected;
	state.correctionState = GnssCorrectionState::Flowing;
	overlay.updateState(state);

	QVERIFY(overlay.accessibleName().contains(QStringLiteral("waiting for receiver data"),
	                                          Qt::CaseInsensitive));
	QVERIFY(!overlay.accessibleName().contains(QStringLiteral("fixed"),
	                                           Qt::CaseInsensitive));

	QImage rendered(overlay.size(), QImage::Format_ARGB32_Premultiplied);
	rendered.fill(Qt::transparent);
	QPainter painter(&rendered);
	overlay.render(&painter);
	painter.end();
	QVERIFY(!rendered.isNull());
	QVERIFY(rendered.pixelColor(rendered.width() / 2, rendered.height() / 2).alpha() > 0);

	QSignalSpy clicked(&overlay, &GnssStatusOverlay::clicked);
	QTest::mouseClick(&overlay, Qt::LeftButton, Qt::NoModifier,
	                  overlay.rect().center());
	QCOMPARE(clicked.count(), 1);

	state.receiverBytesReceived = 128;
	state.protocol = GnssProtocol::UBX;
	state.solution.hasFreshPosition = true;
	state.solution.position.fixType = GnssFixType::RtkFixed;
	state.solution.position.hAccuracyP95 = 0.03f;
	state.solution.position.satellitesUsed = 19;
	overlay.updateState(state);
	QVERIFY(overlay.accessibleName().contains(QStringLiteral("RTK fixed")));
}


void GnssUiTest::detailPanelDiagnosesAndReconfigures()
{
	GnssDetailPanel panel;
	GnssState state;
	state.transportState = GnssTransportState::Connected;
	state.correctionState = GnssCorrectionState::Flowing;
	state.ntripBytesReceived = 4096;
	state.ntripBytesSentToReceiver = 2048;
	state.ntripBytesDroppedToReceiver = 512;
	panel.updateState(state);

	auto* summary = panel.findChild<QLabel*>(QStringLiteral("gnssStatusSummary"));
	auto* receiverHealth = panel.findChild<QLabel*>(QStringLiteral("gnssReceiverHealth"));
	QVERIFY(summary);
	QVERIFY(receiverHealth);
	QVERIFY(summary->text().contains(QStringLiteral("no position data"),
	                                 Qt::CaseInsensitive));
	QVERIFY(receiverHealth->text().contains(QStringLiteral("no data stream"),
	                                        Qt::CaseInsensitive));

	auto* changeReceiver =
	  panel.findChild<QPushButton*>(QStringLiteral("gnssChangeReceiver"));
	auto* settings = panel.findChild<QPushButton*>(QStringLiteral("gnssSettings"));
	QVERIFY(changeReceiver);
	QVERIFY(settings);
	QVERIFY(changeReceiver->minimumHeight() > 0);

	QSignalSpy receiverRequested(&panel, &GnssDetailPanel::receiverChangeRequested);
	QSignalSpy settingsRequested(&panel, &GnssDetailPanel::settingsRequested);
	QTest::mouseClick(changeReceiver, Qt::LeftButton);
	QTest::mouseClick(settings, Qt::LeftButton);
	QCOMPARE(receiverRequested.count(), 1);
	QCOMPARE(settingsRequested.count(), 1);
}


void GnssUiTest::settingsPageProvidesMapIndependentPreflight()
{
	GnssSettingsPage page;
	auto* title = page.findChild<QLabel*>(QStringLiteral("gnssPreflightTitle"));
	auto* detail = page.findChild<QLabel*>(QStringLiteral("gnssPreflightDetail"));
	auto* receiver = page.findChild<QLabel*>(QStringLiteral("gnssPreflightReceiver"));
	auto* corrections = page.findChild<QLabel*>(QStringLiteral("gnssPreflightCorrections"));
	auto* start = page.findChild<QPushButton*>(QStringLiteral("gnssPreflightStart"));
	auto* disconnect = page.findChild<QPushButton*>(QStringLiteral("gnssPreflightDisconnect"));
	QVERIFY(title);
	QVERIFY(detail);
	QVERIFY(receiver);
	QVERIFY(corrections);
	QVERIFY(start);
	QVERIFY(disconnect);
	QVERIFY(start->minimumHeight() > 0);

	GnssState state;
	state.transportState = GnssTransportState::Connected;
	state.correctionState = GnssCorrectionState::Flowing;
	state.ntripBytesReceived = 4096;
	state.ntripBytesSentToReceiver = 3072;
	page.updatePreflightState(state);
	QVERIFY(title->text().contains(QStringLiteral("no receiver data"),
	                               Qt::CaseInsensitive));
	QVERIFY(receiver->text().contains(QStringLiteral("no bytes"),
	                                  Qt::CaseInsensitive));
	QVERIFY(corrections->text().contains(QStringLiteral("3072")));
	QVERIFY(disconnect->isEnabled());

	state.receiverBytesReceived = 2048;
	state.protocol = GnssProtocol::UBX;
	state.solution.hasFreshPosition = true;
	state.solution.position.fixType = GnssFixType::RtkFixed;
	state.solution.position.hAccuracyP95 = 0.03f;
	state.solution.position.satellitesUsed = 18;
	page.updatePreflightState(state);
	QVERIFY(title->text().contains(QStringLiteral("RTK fixed")));
	QVERIFY(detail->text().contains(QStringLiteral("track recording"),
	                                Qt::CaseInsensitive));
	QVERIFY(receiver->text().contains(QStringLiteral("UBX")));
}


QTEST_MAIN(GnssUiTest)
