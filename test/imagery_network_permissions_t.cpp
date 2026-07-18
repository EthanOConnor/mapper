/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery_network_permissions_t.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "imagery/imagery_network_permissions.h"
#include "imagery/tile_network_manager.h"

using namespace OpenOrienteering;

namespace {

imagery::TileNetworkManager::Config configFor(
	const QTemporaryDir& directory,
	const QString& name)
{
	imagery::TileNetworkManager::Config config;
	config.cache_directory =
		directory.filePath(name);
	config.max_retries = 0;
	return config;
}

}  // namespace


void ImageryNetworkPermissionsTest::
	tracksBlockedOriginsAndPersistsExplicitDecisions()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(
		QSettings::IniFormat,
		QSettings::UserScope,
		directory.path());
	QCoreApplication::setOrganizationName(
		QStringLiteral("OpenOrienteeringTest"));
	QCoreApplication::setApplicationName(
		QStringLiteral("ImageryNetworkPermissionsTest"));
	QSettings {}.clear();

	auto const rejected_url = QUrl(
		QStringLiteral("https://split-dns.example.test:8443/tiles/0/0/0"));
	auto const origin =
		imagery::TileNetworkManager::canonicalOrigin(rejected_url);
	{
		imagery::TileNetworkManager network(
			configFor(directory, QStringLiteral("cache-one")));
		imagery::ImageryNetworkPermissions permissions(network);
		QSignalSpy pending(
			&permissions,
			&imagery::ImageryNetworkPermissions::pendingOriginsChanged);

		imagery::TileNetworkResult rejected;
		rejected.outcome =
			imagery::TileNetworkResult::Outcome::Rejected;
		rejected.private_network_rejected = true;
		rejected.private_network_rejected_url = rejected_url;
		network.finished(1, rejected);
		QCOMPARE(pending.size(), 1);
		QCOMPARE(
			permissions.pendingOrigins(),
			QStringList { origin });
		QVERIFY(permissions.approve(rejected_url));
		QVERIFY(permissions.pendingOrigins().isEmpty());
		QCOMPARE(
			permissions.approvedOrigins(),
			QStringList { origin });
		QVERIFY(network.isPrivateOriginApproved(rejected_url));

		rejected.private_network_permission_revoked = true;
		network.finished(2, rejected);
		QVERIFY(permissions.pendingOrigins().isEmpty());
	}

	{
		imagery::TileNetworkManager network(
			configFor(directory, QStringLiteral("cache-two")));
		imagery::ImageryNetworkPermissions permissions(network);
		QCOMPARE(
			permissions.approvedOrigins(),
			QStringList { origin });
		QVERIFY(network.isPrivateOriginApproved(rejected_url));
		QVERIFY(permissions.revoke(rejected_url));
		QVERIFY(permissions.approvedOrigins().isEmpty());
		QVERIFY(!network.isPrivateOriginApproved(rejected_url));

		imagery::TileNetworkResult rejected;
		rejected.outcome =
			imagery::TileNetworkResult::Outcome::Rejected;
		rejected.private_network_rejected = true;
		rejected.private_network_rejected_url = rejected_url;
		network.finished(2, rejected);
		QCOMPARE(
			permissions.pendingOrigins(),
			QStringList { origin });
		QVERIFY(permissions.dismissPending(rejected_url));
		QVERIFY(permissions.pendingOrigins().isEmpty());

		for (auto i = 0; i < 130; ++i)
		{
			auto const candidate = QUrl(
				QStringLiteral(
					"https://user:secret@review-%1.example.test/tiles"
					"?token=secret#fragment")
					.arg(i));
			rejected.private_network_rejected_url = candidate;
			network.finished(3 + i, rejected);
		}
		auto const pending_origins = permissions.pendingOrigins();
		QCOMPARE(pending_origins.size(), 128);
		QVERIFY(!pending_origins.contains(QStringLiteral(
			"https://review-0.example.test:443")));
		QVERIFY(!pending_origins.contains(QStringLiteral(
			"https://review-1.example.test:443")));
		QVERIFY(pending_origins.contains(QStringLiteral(
			"https://review-129.example.test:443")));
		for (auto const& pending_origin : pending_origins)
		{
			QVERIFY(!pending_origin.contains(QLatin1Char('@')));
			QVERIFY(!pending_origin.contains(QLatin1Char('?')));
			QVERIFY(!pending_origin.contains(QLatin1Char('#')));
			QVERIFY(pending_origin.indexOf(QLatin1Char('/'), 8) < 0);
		}
	}

	QSettings {}.clear();
	QSettings {}.setValue(
		QStringLiteral("onlineImagery/approvedPrivateOrigins"),
		QStringList {
			QStringLiteral(
				"https://legacy.example.test:8443/tiles"
				"?token=secret#fragment"),
			QStringLiteral(
				"https://user:secret@invalid.example.test/tiles"),
		});
	{
		imagery::TileNetworkManager network(
			configFor(directory, QStringLiteral("cache-three")));
		imagery::ImageryNetworkPermissions permissions(network);
		auto const canonical = QStringLiteral(
			"https://legacy.example.test:8443");
		QCOMPARE(
			permissions.approvedOrigins(),
			QStringList { canonical });
		QCOMPARE(
			QSettings {}
				.value(QStringLiteral(
					"onlineImagery/approvedPrivateOrigins"))
				.toStringList(),
			QStringList { canonical });
	}

	QSettings {}.clear();
}

void ImageryNetworkPermissionsTest::persistsIpv6OriginsExactly()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(
		QSettings::IniFormat,
		QSettings::UserScope,
		directory.path());
	QCoreApplication::setOrganizationName(
		QStringLiteral("OpenOrienteeringTest"));
	QCoreApplication::setApplicationName(
		QStringLiteral("ImageryNetworkPermissionsIpv6Test"));
	QSettings {}.clear();

	auto const first = QUrl(
		QStringLiteral("https://[fd00::1]:8443/tiles"));
	auto const second = QUrl(
		QStringLiteral("https://[fd00::2]:8443/tiles"));
	auto const first_origin =
		QStringLiteral("https://[fd00::1]:8443");
	{
		imagery::TileNetworkManager network(
			configFor(directory, QStringLiteral("ipv6-one")));
		imagery::ImageryNetworkPermissions permissions(network);
		QVERIFY(permissions.approve(first));
		QCOMPARE(
			permissions.approvedOrigins(),
			QStringList { first_origin });
		QVERIFY(network.isPrivateOriginApproved(first));
		QVERIFY(!network.isPrivateOriginApproved(second));
	}
	{
		imagery::TileNetworkManager network(
			configFor(directory, QStringLiteral("ipv6-two")));
		imagery::ImageryNetworkPermissions permissions(network);
		QCOMPARE(
			permissions.approvedOrigins(),
			QStringList { first_origin });
		QVERIFY(network.isPrivateOriginApproved(first));
		QVERIFY(!network.isPrivateOriginApproved(second));
		QVERIFY(permissions.revoke(first));
		QVERIFY(permissions.approvedOrigins().isEmpty());
	}
	QSettings {}.clear();
}


QTEST_GUILESS_MAIN(ImageryNetworkPermissionsTest)
