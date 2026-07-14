/*
 *    Copyright 2017 Kai Pastor
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


#include "file_dialog.h"

#include <algorithm>
#include <iterator>

#include <QtGlobal>
#include <QStringView>  // IWYU pragma: keep
#include <QVector>


namespace
{
	constexpr int max_filter_length = 100;
	
}  // namespace



namespace OpenOrienteering {

void FileDialog::adjustParameters(QStringView filter, QFileDialog::Options& options)
{
	using std::begin;
	using std::end;
	
	static constexpr auto separator = QStringView{u";;"};
	const auto filters = filter.split(separator);
	
	bool has_long_filters = std::any_of(begin(filters), end(filters), [](auto&& item) {
		return item.length() > max_filter_length;
	});
	
	if (has_long_filters)
		options |= QFileDialog::HideNameFilterDetails;
}


}  // namespace OpenOrienteering
