/*
 *    Copyright 2013-2015 Kai Pastor
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


#ifdef QT_PRINTSUPPORT_LIB

#include "print_progress_dialog.h"

#include <QtGlobal>
#include <QApplication>
#include <QMessageBox>

#include "core/map_printer.h"


namespace OpenOrienteering {

PrintProgressDialog::PrintProgressDialog(MapPrinter* map_printer, QWidget* parent, Qt::WindowFlags f)
 : QProgressDialog(parent, f)
 , map_printer(map_printer)
{
	setWindowModality(Qt::ApplicationModal); // Required for OSX, cf. QTBUG-40112
	setRange(0, 100);
	setMinimumDuration(0);
	setAutoReset(false);
	setAutoClose(false);
	setValue(0);
	
	Q_ASSERT(map_printer);
	connect(map_printer, &MapPrinter::printProgress, this, &PrintProgressDialog::setProgress);
	connect(this, &PrintProgressDialog::canceled, map_printer, &MapPrinter::cancelPrintMap);
}

PrintProgressDialog::~PrintProgressDialog()
{
	// nothing, not inlined
}

void PrintProgressDialog::paintRequested(QPrinter* printer)
{
	reset();
	setValue(0);
	if (!map_printer->printMap(printer))
	{
		if (wasCanceled() || map_printer->outputWasCanceled())
			return;
		QMessageBox::warning(
		  parentWidget(), tr("Printing", "PrintWidget"),
		  map_printer->outputError().isEmpty()
		    ? tr("An error occurred during processing.", "PrintWidget")
		    : map_printer->outputError(),
		  QMessageBox::Ok, QMessageBox::Ok );
	}
}

void PrintProgressDialog::setProgress(int value, const QString& status)
{
	setLabelText(status);
	setValue(value);
	if (value >= maximum())
	{
		hide();
	}
	else if (!isVisible())
	{
		show();
	}
	
	// The dialog is application-modal, so accepting all events lets its Cancel
	// button work without exposing the rest of the UI to re-entrant input.
	QApplication::processEvents(QEventLoop::AllEvents, 100 /* ms */);
}


}  // namespace OpenOrienteering

#endif
