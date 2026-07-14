/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 */

#ifndef OPENORIENTEERING_DOCUMENT_PATH_T_H
#define OPENORIENTEERING_DOCUMENT_PATH_T_H

#include <QObject>

class DocumentPathTest : public QObject
{
Q_OBJECT
private slots:
	void localPathRoundTrip();
	void contentUriRoundTrip();
	void autosaveLocation();
};

#endif
