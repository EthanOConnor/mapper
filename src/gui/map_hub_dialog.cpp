/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_dialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCommandLinkButton>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScroller>
#include <QSettings>
#include <QSize>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "collaboration/managed_map_workspace.h"
#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_credentials.h"
#include "collaboration/map_hub_imagery_catalog.h"
#include "collaboration/map_hub_workspace.h"
#include "core/document_path.h"
#include "gui/action_icon.h"
#include "gui/main_window.h"
#include "imagery/tile_network_manager.h"
#include "settings.h"

namespace OpenOrienteering {

namespace {

constexpr int id_role = Qt::UserRole;
constexpr int project_id_role = Qt::UserRole + 1;
constexpr int status_role = Qt::UserRole + 2;
constexpr int package_type_role = Qt::UserRole + 3;
constexpr int item_kind_role = Qt::UserRole + 4;
constexpr int web_url_role = Qt::UserRole + 5;
constexpr int title_role = Qt::UserRole + 6;

bool assignmentCanStart(const QTreeWidgetItem *item) {
  if (!item)
    return false;
  const auto status = item->data(0, status_role).toString();
  const auto package_type = item->data(0, package_type_role).toString();
  return MapHubApiClient::isMapperWorkspacePackageType(package_type) &&
         (status == QLatin1String("offered") ||
          status == QLatin1String("accepted") ||
          status == QLatin1String("active"));
}

QString safeFileName(QString title) {
  title.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}._ -]+")),
                QStringLiteral("-"));
  title = title.simplified();
  if (title.isEmpty() || title == QLatin1String(".") ||
      title == QLatin1String(".."))
    return QStringLiteral("connected-map");
  // Leave room for stable IDs, revision numbers, and collision suffixes even
  // when every Unicode code point occupies four UTF-8 bytes.
  if (title.size() > 44)
    title.truncate(44);
  return title;
}

QString shortStableId(QString id) {
  id.remove(QLatin1Char('-'));
  id.remove(QLatin1Char('{'));
  id.remove(QLatin1Char('}'));
  return id.left(12).toLower();
}

QString artifactExtension(const QJsonObject &revision) {
  auto kind = revision.value(QStringLiteral("artifact_kind")).toString();
  if (kind == QLatin1String("ocad"))
    return QStringLiteral("ocd");
  if (kind == QLatin1String("omap"))
    return QStringLiteral("omap");
  auto suffix =
      QFileInfo(revision.value(QStringLiteral("original_name")).toString())
          .suffix()
          .toLower();
  return suffix == QLatin1String("ocd") ? suffix : QStringLiteral("omap");
}

QString projectManifestUrl(const QString &server, const QString &project_id) {
  auto url = QUrl::fromUserInput(server).adjusted(QUrl::StripTrailingSlash);
  url.setPath(QStringLiteral("/api/v1/projects/%1/manifest").arg(project_id));
  url.setQuery(QString{});
  url.setFragment(QString{});
  return url.toString(QUrl::FullyEncoded);
}

QString eventWebUrl(const QString &server, const QString &event_id) {
  auto url = QUrl::fromUserInput(server).adjusted(QUrl::StripTrailingSlash);
  url.setPath(QStringLiteral("/events/%1/").arg(event_id));
  url.setQuery(QString{});
  url.setFragment(QString{});
  return url.toString(QUrl::FullyEncoded);
}

class ConnectedMapDialog final : public QDialog {
public:
  explicit ConnectedMapDialog(const QJsonObject &library, QWidget *parent)
      : QDialog(parent), title(new QLineEdit(this)), kind(new QComboBox(this)),
        venues(new QListWidget(this)), predecessors(new QListWidget(this)),
        lineage(new QComboBox(this)), package_title(new QLineEdit(this)),
        work_type(new QComboBox(this)), assignee(new QComboBox(this)),
        crs(new QLineEdit(this)), scale(new QSpinBox(this)),
        standard(new QLineEdit(this)), source_title(new QLineEdit(this)),
        source_type(new QComboBox(this)), source_provider(new QLineEdit(this)),
        source_crs(new QLineEdit(this)), source_resolution(new QLineEdit(this)),
        exclusive(new QCheckBox(tr("Use an exclusive editing lease"), this)) {
    setWindowTitle(tr("New connected map"));
#if defined(MAPPER_MOBILE)
    if (parent) {
      setAttribute(Qt::WA_WindowPropagation);
      setPalette(parent->palette());
      resize(parent->size());
    }
    auto mobile_font = font();
    mobile_font.setPointSizeF(16.0);
    setFont(mobile_font);
    setStyleSheet(QStringLiteral(
        "QDialog { background: palette(window); }"
        "QLineEdit, QComboBox, QSpinBox, QListWidget { background: "
        "palette(base); border: 1px solid palette(midlight); "
        "border-radius: 8px; padding: 6px 8px; }"
        "QListWidget::item { padding: 7px 5px; }"
        "QLabel#connectedMapSection { color: palette(highlight); "
        "padding-top: 12px; }"
        "QPushButton#connectedMapPrimary { background: palette(highlight); "
        "color: palette(highlighted-text); border: 0; border-radius: 12px; "
        "padding: 10px 14px; }"));
#else
    resize(720, 780);
#endif
    auto *form_widget = new QWidget(this);
    auto *form = new QFormLayout(form_widget);
#if defined(MAPPER_MOBILE)
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
#endif
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
#if defined(MAPPER_MOBILE)
    auto add_section = [form](const QString &text) {
      auto *heading = new QLabel(text);
      auto font = heading->font();
      font.setBold(true);
      font.setPointSizeF(font.pointSizeF() * 1.2);
      heading->setFont(font);
      heading->setObjectName(QStringLiteral("connectedMapSection"));
      form->addRow(heading);
    };
    auto *intro = new QLabel(
        tr("Create the Map Hub project, its first assignment, and the local "
           "workspace as one guided setup."),
        form_widget);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color: palette(mid);"));
    form->addRow(intro);
    add_section(tr("Map"));
#endif
    form->addRow(tr("Map name:"), title);
    kind->addItem(tr("New map"), QStringLiteral("new_map"));
    kind->addItem(tr("Remap"), QStringLiteral("remap"));
    kind->addItem(tr("Combined map"), QStringLiteral("combination"));
    kind->addItem(tr("Expansion"), QStringLiteral("expansion"));
    kind->addItem(tr("Update"), QStringLiteral("update"));
    form->addRow(tr("Workflow:"), kind);
    venues->setSelectionMode(QAbstractItemView::ExtendedSelection);
    venues->setMaximumHeight(105);
    for (const auto value : library.value(QStringLiteral("venues")).toArray()) {
      auto object = value.toObject();
      auto label = object.value(QStringLiteral("name")).toString();
      auto city = object.value(QStringLiteral("city")).toString();
      if (!city.isEmpty())
        label += QStringLiteral(" — ") + city;
      auto *item = new QListWidgetItem(label, venues);
      item->setData(id_role, object.value(QStringLiteral("id")).toString());
    }
    if (venues->count() == 1)
      venues->item(0)->setSelected(true);
    form->addRow(tr("Venue(s):"), venues);

#if defined(MAPPER_MOBILE)
    add_section(tr("Relationship to existing maps"));
#endif
    predecessors->setSelectionMode(QAbstractItemView::ExtendedSelection);
    predecessors->setMaximumHeight(105);
    for (const auto value :
         library.value(QStringLiteral("projects")).toArray()) {
      auto object = value.toObject();
      QStringList venue_names;
      for (const auto venue_name :
           object.value(QStringLiteral("venue_names")).toArray())
        venue_names.append(venue_name.toString());
      auto label = object.value(QStringLiteral("title")).toString();
      if (!venue_names.isEmpty())
        label += QStringLiteral(" — ") + venue_names.join(QStringLiteral(", "));
      auto *item = new QListWidgetItem(label, predecessors);
      item->setData(id_role, object.value(QStringLiteral("id")).toString());
    }
    form->addRow(tr("Prior map(s):"), predecessors);
    lineage->addItem(tr("Incorporates as reference"),
                     QStringLiteral("reference"));
    lineage->addItem(tr("Combines"), QStringLiteral("combines"));
    lineage->addItem(tr("Supersedes"), QStringLiteral("supersedes"));
    lineage->addItem(tr("Partially supersedes"),
                     QStringLiteral("partially_supersedes"));
    lineage->addItem(tr("Expands"), QStringLiteral("expands"));
    lineage->addItem(tr("Derived from"), QStringLiteral("derived"));
    lineage->addItem(tr("Overlaps"), QStringLiteral("overlaps"));
    form->addRow(tr("Relationship to prior map(s):"), lineage);

#if defined(MAPPER_MOBILE)
    add_section(tr("First assignment"));
#endif
    package_title->setText(tr("Prepare the basemap"));
    form->addRow(tr("First work package:"), package_title);
    work_type->addItem(tr("Basemap preparation"), QStringLiteral("basemap"));
    work_type->addItem(tr("New field mapping"), QStringLiteral("new_mapping"));
    work_type->addItem(tr("Remap"), QStringLiteral("remap"));
    work_type->addItem(tr("Map update"), QStringLiteral("update"));
    work_type->addItem(tr("Field check"), QStringLiteral("field_check"));
    form->addRow(tr("Work type:"), work_type);
    assignee->addItem(tr("Me (this connected account)"), QString{});
    auto current_person_id =
        library.value(QStringLiteral("current_person_id")).toString();
    auto can_assign_others = library.value(QStringLiteral("capabilities"))
                                 .toObject()
                                 .value(QStringLiteral("can_assign_others"))
                                 .toBool();
    if (can_assign_others) {
      for (const auto value :
           library.value(QStringLiteral("people")).toArray()) {
        auto person = value.toObject();
        if (person.value(QStringLiteral("id")).toString() == current_person_id)
          continue;
        assignee->addItem(
            person.value(QStringLiteral("display_name")).toString(),
            person.value(QStringLiteral("id")).toString());
      }
    }
    form->addRow(tr("Assign first work to:"), assignee);
#if defined(MAPPER_MOBILE)
    add_section(tr("Technical target"));
#endif
    crs->setPlaceholderText(QStringLiteral("EPSG:6596"));
    form->addRow(tr("Target CRS:"), crs);
    scale->setRange(100, 1000000);
    scale->setValue(10000);
    scale->setSingleStep(500);
    form->addRow(tr("Map scale:"), scale);
    standard->setPlaceholderText(QStringLiteral("ISOM 2017-2"));
    form->addRow(tr("Symbol standard:"), standard);
#if defined(MAPPER_MOBILE)
    add_section(tr("Primary source dataset — optional"));
    auto *source_help = new QLabel(
        tr("Leave the dataset name empty when the source is not known yet."),
        form_widget);
    source_help->setWordWrap(true);
    source_help->setStyleSheet(QStringLiteral("color: palette(mid);"));
    form->addRow(source_help);
#endif
    source_title->setPlaceholderText(
        tr("Optional, e.g. 2025 King County LiDAR"));
    form->addRow(tr("Primary source dataset:"), source_title);
    source_type->addItem(tr("LiDAR"), QStringLiteral("lidar"));
    source_type->addItem(tr("Aerial imagery"), QStringLiteral("imagery"));
    source_type->addItem(tr("Legacy map"), QStringLiteral("legacy_map"));
    source_type->addItem(tr("Scan"), QStringLiteral("scan"));
    source_type->addItem(tr("Survey"), QStringLiteral("survey"));
    source_type->addItem(tr("OpenStreetMap"), QStringLiteral("osm"));
    source_type->addItem(tr("Other"), QStringLiteral("other"));
    form->addRow(tr("Source type:"), source_type);
    form->addRow(tr("Source provider:"), source_provider);
    source_crs->setPlaceholderText(QStringLiteral("EPSG:6596"));
    form->addRow(tr("Source horizontal CRS:"), source_crs);
    source_resolution->setPlaceholderText(tr("e.g. 3 ft or 1 m"));
    form->addRow(tr("Source resolution:"), source_resolution);
#if defined(MAPPER_MOBILE)
    for (auto *widget : {static_cast<QWidget *>(source_type),
                         static_cast<QWidget *>(source_provider),
                         static_cast<QWidget *>(source_crs),
                         static_cast<QWidget *>(source_resolution)})
      widget->setEnabled(false);
    connect(source_title, &QLineEdit::textChanged, this,
            [this](const QString &text) {
              const auto enabled = !text.trimmed().isEmpty();
              source_type->setEnabled(enabled);
              source_provider->setEnabled(enabled);
              source_crs->setEnabled(enabled);
              source_resolution->setEnabled(enabled);
            });
    for (auto *widget : findChildren<QLineEdit *>())
      widget->setMinimumHeight(42);
    for (auto *widget : findChildren<QComboBox *>())
      widget->setMinimumHeight(42);
#endif
    exclusive->setChecked(true);
    form->addRow(exclusive);
    auto *note =
        new QLabel(tr("The Map Hub project and audit record are created before "
                      "Mapper creates the local .omap workspace."),
                   this);
    note->setWordWrap(true);
#if defined(MAPPER_MOBILE)
    note->setStyleSheet(QStringLiteral("color: palette(mid);"));
#endif
    form->addRow(note);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
#if defined(MAPPER_MOBILE)
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create in Map Hub"));
    buttons->button(QDialogButtonBox::Ok)->setMinimumHeight(44);
    buttons->button(QDialogButtonBox::Ok)
        ->setObjectName(QStringLiteral("connectedMapPrimary"));
#endif
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
      if (title->text().trimmed().isEmpty() ||
          venues->selectedItems().isEmpty() ||
          package_title->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("New connected map"),
                             tr("Choose a map name, at least one venue, and a "
                                "first work package."));
        return;
      }
      auto workflow = kind->currentData().toString();
      auto prior_count = predecessors->selectedItems().size();
      if (workflow == QLatin1String("combination") && prior_count < 2) {
        QMessageBox::warning(
            this, tr("New connected map"),
            tr("A combined map must identify at least two prior maps."));
        return;
      }
      if ((workflow == QLatin1String("remap") ||
           workflow == QLatin1String("expansion") ||
           workflow == QLatin1String("update")) &&
          prior_count == 0) {
        QMessageBox::warning(
            this, tr("New connected map"),
            tr("This workflow must identify the prior map it changes."));
        return;
      }
      accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
#if defined(MAPPER_MOBILE)
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
#endif
    QScroller::grabGesture(scroll->viewport(), QScroller::TouchGesture);
    scroll->setWidget(form_widget);
    layout->addWidget(scroll, 1);
    layout->addWidget(buttons);

    connect(kind, &QComboBox::currentIndexChanged, this, [this] {
      auto value = kind->currentData().toString();
      if (value == QLatin1String("combination"))
        lineage->setCurrentIndex(lineage->findData(QStringLiteral("combines")));
      else if (value == QLatin1String("expansion"))
        lineage->setCurrentIndex(lineage->findData(QStringLiteral("expands")));
      else if (value == QLatin1String("remap"))
        lineage->setCurrentIndex(
            lineage->findData(QStringLiteral("supersedes")));
      else if (value == QLatin1String("update"))
        lineage->setCurrentIndex(
            lineage->findData(QStringLiteral("partially_supersedes")));
    });
  }

  QJsonObject payload() const {
    QJsonArray venue_ids;
    for (const auto *item : venues->selectedItems())
      venue_ids.append(item->data(id_role).toString());
    QJsonArray predecessor_ids;
    for (const auto *item : predecessors->selectedItems())
      predecessor_ids.append(item->data(id_role).toString());
    QJsonObject work{
        {QStringLiteral("title"), package_title->text().trimmed()},
        {QStringLiteral("type"), work_type->currentData().toString()},
        {QStringLiteral("exclusive_editing"), exclusive->isChecked()},
    };
    if (!assignee->currentData().toString().isEmpty())
      work.insert(QStringLiteral("assignee_id"),
                  assignee->currentData().toString());
    QJsonObject result{
        {QStringLiteral("title"), title->text().trimmed()},
        {QStringLiteral("kind"), kind->currentData().toString()},
        {QStringLiteral("venue_ids"), venue_ids},
        {QStringLiteral("predecessor_ids"), predecessor_ids},
        {QStringLiteral("lineage_relationship"),
         lineage->currentData().toString()},
        {QStringLiteral("target"),
         QJsonObject{
             {QStringLiteral("crs"), crs->text().trimmed()},
             {QStringLiteral("scale"), scale->value()},
             {QStringLiteral("symbol_standard"), standard->text().trimmed()},
         }},
        {QStringLiteral("access_class"), QStringLiteral("members")},
        {QStringLiteral("work_package"), work},
    };
    if (!source_title->text().trimmed().isEmpty()) {
      result.insert(
          QStringLiteral("source_dataset"),
          QJsonObject{
              {QStringLiteral("title"), source_title->text().trimmed()},
              {QStringLiteral("dataset_type"),
               source_type->currentData().toString()},
              {QStringLiteral("provider"), source_provider->text().trimmed()},
              {QStringLiteral("horizontal_crs"), source_crs->text().trimmed()},
              {QStringLiteral("resolution"),
               source_resolution->text().trimmed()},
          });
    }
    return result;
  }

  QString mapTitle() const { return title->text().trimmed(); }
  ManagedMapWorkspace workspaceDefaults() const {
    ManagedMapWorkspace defaults;
    defaults.target_crs = crs->text().trimmed();
    defaults.target_scale = scale->value();
    defaults.symbol_standard = standard->text().trimmed();
    return defaults;
  }
  bool startLocally() const {
    return assignee->currentData().toString().isEmpty();
  }
  QString assigneeName() const { return assignee->currentText(); }

private:
  QLineEdit *title;
  QComboBox *kind;
  QListWidget *venues;
  QListWidget *predecessors;
  QComboBox *lineage;
  QLineEdit *package_title;
  QComboBox *work_type;
  QComboBox *assignee;
  QLineEdit *crs;
  QSpinBox *scale;
  QLineEdit *standard;
  QLineEdit *source_title;
  QComboBox *source_type;
  QLineEdit *source_provider;
  QLineEdit *source_crs;
  QLineEdit *source_resolution;
  QCheckBox *exclusive;
};

} // namespace

MapHubDialog::MapHubDialog(MainWindow *window)
    : QDialog(window), window(window), client(nullptr),
      pages(new QStackedWidget(this)), first_use_page(new QWidget(this)),
      library_page(new QWidget(this)),
      first_use_status(new QLabel(first_use_page)),
      first_use_server(new QLineEdit(first_use_page)),
      first_use_workspace(new QLineEdit(first_use_page)),
      first_use_token(new QLineEdit(first_use_page)),
      first_use_invite(new QLineEdit(first_use_page)),
      first_use_account_tabs(new QTabWidget(first_use_page)),
      passkey_timer(new QTimer(this)),
      first_use_browse(new QPushButton(tr("Choose…"), first_use_page)),
      connect_button(
          new QPushButton(tr("Connect and open Map Hub"), first_use_page)),
      invitation_button(
          new QPushButton(tr("Set up account in browser…"), first_use_page)),
      connection_label(new QLabel(this)), activity_label(new QLabel(this)),
      tabs(new QTabWidget(this)), assignment_list(new QTreeWidget(this)),
      project_list(new QTreeWidget(this)), event_list(new QTreeWidget(this)),
      start_button(new QPushButton(tr("Start selected assignment"), this)),
      open_project_button(
          new QPushButton(tr("Current revision details…"), this)),
      open_event_button(new QPushButton(tr("Open event in Map Hub…"), this)),
      new_button(new QPushButton(tr("New connected map…"), this)),
      refresh_button(new QPushButton(tr("Refresh"), this)) {
  setWindowTitle(tr("Map Hub — library and assignments"));
#if defined(MAPPER_MOBILE)
  if (window) {
    // A QDialog is a window, so Qt does not normally inherit its parent's
    // palette.  iOS supplies the dark window surface independently; without
    // explicit propagation, controls can retain light-theme (black) text.
    setAttribute(Qt::WA_WindowPropagation);
    setPalette(window->palette());
    resize(window->size());
  }
  auto mobile_font = font();
  mobile_font.setPointSizeF(16.0);
  setFont(mobile_font);
  first_use_page->setObjectName(QStringLiteral("mapHubFirstUse"));
  first_use_page->setStyleSheet(QStringLiteral(
      "QWidget#mapHubFirstUse { background: palette(window); }"
      "QFrame#mapHubChoiceGroup, QFrame#mapHubConnectionCard { "
      "background: transparent; border: 0; }"
      "QCommandLinkButton#mapHubChoice { background: transparent; border: 0; "
      "padding: 7px 2px; text-align: left; }"
      "QCommandLinkButton#mapHubChoice:pressed { background: "
      "palette(alternate-base); }"
      "QToolButton#mapHubBack { color: palette(link); border: 0; "
      "padding: 8px 2px; text-align: left; }"
      "QLineEdit { background: palette(base); border: 1px solid "
      "palette(midlight); border-radius: 10px; padding: 8px 10px; }"
      "QPushButton#mapHubPrimary { background: palette(highlight); color: "
      "palette(highlighted-text); border: 0; border-radius: 12px; "
      "padding: 10px 14px; }"
      "QPushButton#mapHubQuiet { background: transparent; border: 0; "
      "color: palette(link); padding: 8px; text-align: left; }"
      "QWidget#mapHubFooter { background: palette(base); border-top: 1px "
      "solid palette(midlight); }"));
  library_page->setObjectName(QStringLiteral("mapHubLibrary"));
  library_page->setStyleSheet(QStringLiteral(
      "QWidget#mapHubLibrary { background: palette(window); }"
      "QTabWidget::pane { background: transparent; border: 0; }"
      "QTabBar::tab { background: transparent; padding: 9px 12px; "
      "border: 0; color: palette(mid); }"
      "QTabBar::tab:selected { color: palette(text); border-bottom: 3px "
      "solid palette(highlight); }"
      "QTreeWidget { background: transparent; border: 0; }"
      "QPushButton#mapHubLibraryPrimary { background: palette(highlight); "
      "color: palette(highlighted-text); border: 0; border-radius: 12px; "
      "padding: 10px 14px; }"
      "QPushButton#mapHubLibraryQuiet { background: palette(base); border: "
      "1px solid palette(midlight); border-radius: 10px; padding: 8px; }"));
#else
  resize(880, 640);
#endif
  pages->setMinimumSize({});
  pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

  auto *first_use_title = new QLabel(tr("Connect to Map Hub"), first_use_page);
  auto title_font = first_use_title->font();
  title_font.setPointSizeF(title_font.pointSizeF() * 1.65);
  title_font.setBold(true);
  first_use_title->setFont(title_font);
  auto *first_use_intro = new QLabel(
      tr("Sign in securely with your Map Hub passkey, use an invitation, or "
         "connect with a token."),
      first_use_page);
  first_use_intro->setWordWrap(true);
  first_use_intro->setStyleSheet(QStringLiteral("color: palette(mid);"));
  first_use_status->setWordWrap(true);

  first_use_server->setPlaceholderText(
      QStringLiteral("https://maps.example.org"));
  auto *workspace_row = new QWidget(first_use_page);
  auto *workspace_layout = new QHBoxLayout(workspace_row);
  workspace_layout->setContentsMargins({});
  workspace_layout->addWidget(first_use_workspace, 1);
  workspace_layout->addWidget(first_use_browse);
#if defined(MAPPER_MOBILE)
  first_use_account_tabs->hide();
  first_use_flow = new QStackedWidget(first_use_page);

  auto make_back_button = [this](QWidget *parent) {
    auto *back = new QToolButton(parent);
    back->setText(tr("‹ Map Hub"));
    back->setObjectName(QStringLiteral("mapHubBack"));
    back->setToolButtonStyle(Qt::ToolButtonTextOnly);
    back->setAutoRaise(true);
    connect(back, &QToolButton::clicked, this,
            [this] { first_use_flow->setCurrentIndex(0); });
    return back;
  };
  auto make_flow_title = [](const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    auto font = label->font();
    font.setPointSizeF(font.pointSizeF() * 1.55);
    font.setBold(true);
    label->setFont(font);
    return label;
  };

  auto *landing_page = new QWidget(first_use_flow);
  auto *landing_layout = new QVBoxLayout(landing_page);
  landing_layout->setContentsMargins(20, 18, 20, 18);
  landing_layout->setSpacing(12);
  landing_layout->addWidget(first_use_title);
  landing_layout->addWidget(first_use_intro);
  landing_layout->addSpacing(10);
  auto *passkey_choice = new QCommandLinkButton(
      tr("Sign in with passkey"),
      tr("Open Map Hub in Safari, sign in, and approve this iPhone."),
      landing_page);
  passkey_choice->setObjectName(QStringLiteral("mapHubChoice"));
  auto choice_font = passkey_choice->font();
  choice_font.setPointSizeF(18.0);
  passkey_choice->setFont(choice_font);
  passkey_choice->setIcon(ActionIcon::fromName(u"arrow-right"));
  passkey_choice->setIconSize(QSize(24, 24));
  passkey_choice->setMinimumHeight(80);
  auto *invitation_choice = new QCommandLinkButton(
      tr("Use an invitation"),
      tr("Create or join an account using a token from your map librarian."),
      landing_page);
  invitation_choice->setObjectName(QStringLiteral("mapHubChoice"));
  invitation_choice->setFont(choice_font);
  invitation_choice->setIcon(ActionIcon::fromName(u"arrow-right"));
  invitation_choice->setIconSize(QSize(24, 24));
  invitation_choice->setMinimumHeight(80);
  auto *token_choice = new QCommandLinkButton(
      tr("Connect an existing account"),
      tr("Paste the Mapper connection token from your Map Hub profile."),
      landing_page);
  token_choice->setObjectName(QStringLiteral("mapHubChoice"));
  token_choice->setFont(choice_font);
  token_choice->setIcon(ActionIcon::fromName(u"arrow-right"));
  token_choice->setIconSize(QSize(24, 24));
  token_choice->setMinimumHeight(80);
  auto *choice_group = new QFrame(landing_page);
  choice_group->setObjectName(QStringLiteral("mapHubChoiceGroup"));
  auto *choice_layout = new QVBoxLayout(choice_group);
  choice_layout->setContentsMargins({});
  choice_layout->setSpacing(0);
  choice_layout->addWidget(passkey_choice);
  auto *passkey_separator = new QFrame(choice_group);
  passkey_separator->setFrameShape(QFrame::NoFrame);
  passkey_separator->setFixedHeight(1);
  passkey_separator->setStyleSheet(
      QStringLiteral("background: palette(midlight); border: 0;"));
  choice_layout->addWidget(passkey_separator);
  choice_layout->addWidget(invitation_choice);
  auto *choice_separator = new QFrame(choice_group);
  choice_separator->setFrameShape(QFrame::NoFrame);
  choice_separator->setFixedHeight(1);
  choice_separator->setStyleSheet(
      QStringLiteral("background: palette(midlight); border: 0;"));
  choice_layout->addWidget(choice_separator);
  choice_layout->addWidget(token_choice);
  landing_layout->addWidget(choice_group);
  landing_layout->addSpacing(6);

  auto *connection_card = new QFrame(landing_page);
  connection_card->setObjectName(QStringLiteral("mapHubConnectionCard"));
  auto *connection_card_layout = new QVBoxLayout(connection_card);
  connection_card_layout->setContentsMargins(2, 6, 2, 6);
  connection_card_layout->setSpacing(4);
  auto *connection_heading = new QLabel(tr("Connection"), landing_page);
  auto connection_heading_font = connection_heading->font();
  connection_heading_font.setBold(true);
  connection_heading->setFont(connection_heading_font);
  connection_card_layout->addWidget(connection_heading);
  first_use_connection_summary = new QLabel(landing_page);
  first_use_connection_summary->setWordWrap(true);
  first_use_connection_summary->setStyleSheet(
      QStringLiteral("color: palette(mid);"));
  connection_card_layout->addWidget(first_use_connection_summary);
  auto *change_connection =
      new QPushButton(tr("Change server or workspace"), landing_page);
  change_connection->setObjectName(QStringLiteral("mapHubQuiet"));
  connection_card_layout->addWidget(change_connection);
  landing_layout->addWidget(connection_card);
  landing_layout->addStretch();
  first_use_flow->addWidget(landing_page);

  auto *invitation_page = new QWidget(first_use_flow);
  auto *invitation_layout = new QVBoxLayout(invitation_page);
  invitation_layout->setContentsMargins(20, 12, 20, 18);
  invitation_layout->setSpacing(12);
  invitation_layout->addWidget(make_back_button(invitation_page));
  invitation_layout->addWidget(
      make_flow_title(tr("Use an invitation"), invitation_page));
  auto *invitation_help = new QLabel(
      tr("Paste the invitation from your map librarian. Account setup opens "
         "in Safari and offers a passkey first."),
      invitation_page);
  invitation_help->setWordWrap(true);
  invitation_help->setStyleSheet(QStringLiteral("color: palette(mid);"));
  invitation_layout->addWidget(invitation_help);
  auto *invitation_label = new QLabel(tr("Invitation token"), invitation_page);
  invitation_layout->addWidget(invitation_label);
  first_use_invite->setEchoMode(QLineEdit::Password);
  first_use_invite->setMinimumHeight(44);
  invitation_layout->addWidget(first_use_invite);
  invitation_button->setText(tr("Continue in Safari"));
  invitation_button->setMinimumHeight(48);
  invitation_button->setObjectName(QStringLiteral("mapHubPrimary"));
  invitation_layout->addWidget(invitation_button);
  invitation_layout->addStretch();
  first_use_flow->addWidget(invitation_page);

  auto *token_page = new QWidget(first_use_flow);
  auto *token_layout = new QVBoxLayout(token_page);
  token_layout->setContentsMargins(20, 12, 20, 18);
  token_layout->setSpacing(12);
  token_layout->addWidget(make_back_button(token_page));
  token_layout->addWidget(make_flow_title(tr("Connect account"), token_page));
  auto *token_help = new QLabel(
      tr("Copy the Mapper connection token from your Map Hub profile and "
         "paste it below. It is stored securely on this iPhone."),
      token_page);
  token_help->setWordWrap(true);
  token_help->setStyleSheet(QStringLiteral("color: palette(mid);"));
  token_layout->addWidget(token_help);
  token_layout->addWidget(new QLabel(tr("Connection token"), token_page));
  first_use_token->setEchoMode(QLineEdit::Password);
  first_use_token->setPlaceholderText(tr("Mapper API token"));
  first_use_token->setMinimumHeight(44);
  token_layout->addWidget(first_use_token);
  connect_button->setText(tr("Connect to Map Hub"));
  connect_button->setMinimumHeight(48);
  connect_button->setObjectName(QStringLiteral("mapHubPrimary"));
  token_layout->addWidget(connect_button);
  token_layout->addStretch();
  first_use_flow->addWidget(token_page);

  auto *connection_page = new QWidget(first_use_flow);
  auto *connection_layout = new QVBoxLayout(connection_page);
  connection_layout->setContentsMargins(20, 12, 20, 18);
  connection_layout->setSpacing(12);
  connection_layout->addWidget(make_back_button(connection_page));
  connection_layout->addWidget(
      make_flow_title(tr("Connection details"), connection_page));
  auto *connection_help = new QLabel(
      tr("Most people can keep these defaults. Change them only for another "
         "Map Hub server or workspace location."),
      connection_page);
  connection_help->setWordWrap(true);
  connection_help->setStyleSheet(QStringLiteral("color: palette(mid);"));
  connection_layout->addWidget(connection_help);
  auto *connection_form = new QFormLayout;
  connection_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
  connection_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  connection_form->addRow(tr("Map Hub server"), first_use_server);
  connection_form->addRow(tr("Local map workspaces"), workspace_row);
  connection_layout->addLayout(connection_form);
  auto *connection_done = new QPushButton(tr("Done"), connection_page);
  connection_done->setMinimumHeight(48);
  connection_done->setObjectName(QStringLiteral("mapHubPrimary"));
  connection_layout->addWidget(connection_done);
  connection_layout->addStretch();
  first_use_flow->addWidget(connection_page);

  passkey_page = new QWidget(first_use_flow);
  auto *passkey_layout = new QVBoxLayout(passkey_page);
  passkey_layout->setContentsMargins(20, 12, 20, 18);
  passkey_layout->setSpacing(12);
  auto *passkey_back = make_back_button(passkey_page);
  passkey_layout->addWidget(passkey_back);
  passkey_layout->addWidget(
      make_flow_title(tr("Sign in with passkey"), passkey_page));
  auto *passkey_help = new QLabel(
      tr("Safari will open Map Hub. Sign in with your passkey, confirm the "
         "matching code, then return to Mapper."),
      passkey_page);
  passkey_help->setWordWrap(true);
  passkey_help->setStyleSheet(QStringLiteral("color: palette(mid);"));
  passkey_layout->addWidget(passkey_help);
  auto *code_heading = new QLabel(tr("Confirmation code"), passkey_page);
  passkey_layout->addWidget(code_heading);
  passkey_code = new QLabel(passkey_page);
  auto code_font = passkey_code->font();
  code_font.setPointSizeF(28.0);
  code_font.setBold(true);
  code_font.setLetterSpacing(QFont::AbsoluteSpacing, 3.0);
  passkey_code->setFont(code_font);
  passkey_layout->addWidget(passkey_code);
  passkey_status = new QLabel(passkey_page);
  passkey_status->setWordWrap(true);
  passkey_layout->addWidget(passkey_status);
  passkey_open_browser =
      new QPushButton(tr("Open Map Hub in Safari"), passkey_page);
  passkey_open_browser->setMinimumHeight(48);
  passkey_open_browser->setObjectName(QStringLiteral("mapHubPrimary"));
  passkey_layout->addWidget(passkey_open_browser);
  passkey_layout->addStretch();
  first_use_flow->addWidget(passkey_page);

  connect(passkey_choice, &QCommandLinkButton::clicked, this,
          &MapHubDialog::beginPasskeyConnection);
  connect(passkey_back, &QAbstractButton::clicked, this,
          &MapHubDialog::clearPasskeyConnection);
  connect(passkey_open_browser, &QAbstractButton::clicked, this, [this] {
    if (!passkey_verification_url.isEmpty())
      QDesktopServices::openUrl(passkey_verification_url);
  });
  connect(invitation_choice, &QCommandLinkButton::clicked, this, [this] {
    first_use_flow->setCurrentIndex(1);
    first_use_invite->setFocus();
  });
  connect(token_choice, &QCommandLinkButton::clicked, this, [this] {
    first_use_flow->setCurrentIndex(2);
    first_use_token->setFocus();
  });
  connect(change_connection, &QAbstractButton::clicked, this,
          [this] { first_use_flow->setCurrentIndex(3); });
  connect(connection_done, &QAbstractButton::clicked, this, [this] {
    first_use_connection_summary->setText(
        tr("%1\nMaps stay in %2")
            .arg(first_use_server->text(), first_use_workspace->text()));
    first_use_flow->setCurrentIndex(0);
  });
#else
  auto *connection_form = new QFormLayout;
  connection_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  connection_form->addRow(tr("Map Hub server:"), first_use_server);
  connection_form->addRow(tr("Local map workspaces:"), workspace_row);

  auto *invitation_page = new QWidget(first_use_account_tabs);
  auto *invitation_form = new QFormLayout(invitation_page);
  invitation_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  first_use_invite->setEchoMode(QLineEdit::Password);
  auto *invitation_help = new QLabel(
      tr("Account setup opens in your browser. A passkey is offered first; "
         "you can choose a password there instead."),
      invitation_page);
  invitation_help->setWordWrap(true);
  invitation_form->addRow(invitation_help);
  invitation_form->addRow(tr("Invitation token:"), first_use_invite);
  invitation_form->addRow(invitation_button);
  first_use_account_tabs->addTab(invitation_page, tr("I have an invitation"));

  auto *token_page = new QWidget(first_use_account_tabs);
  auto *token_form = new QFormLayout(token_page);
  token_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  first_use_token->setEchoMode(QLineEdit::Password);
  first_use_token->setPlaceholderText(tr("Mapper API token"));
  auto *token_help = new QLabel(
      tr("Use this if a Map Hub administrator gave you an account token "
         "instead of an invitation."),
      token_page);
  token_help->setWordWrap(true);
  token_form->addRow(token_help);
  token_form->addRow(tr("Account token:"), first_use_token);
  token_form->addRow(connect_button);
  first_use_account_tabs->addTab(token_page,
                                 tr("Paste Mapper connection token"));
#endif

  passkey_timer->setInterval(2000);
  connect(passkey_timer, &QTimer::timeout, this,
          &MapHubDialog::pollPasskeyConnection);

  auto *first_use_close = new QPushButton(tr("Not now"), first_use_page);
#if defined(MAPPER_MOBILE)
  first_use_close->setMinimumHeight(44);
  first_use_close->setObjectName(QStringLiteral("mapHubQuiet"));
  auto *first_use_page_layout = new QVBoxLayout(first_use_page);
  first_use_page_layout->setContentsMargins({});
  first_use_page_layout->setSpacing(0);
  first_use_page_layout->addWidget(first_use_flow, 1);
  auto *mobile_footer = new QWidget(first_use_page);
  mobile_footer->setObjectName(QStringLiteral("mapHubFooter"));
  auto *mobile_footer_layout = new QVBoxLayout(mobile_footer);
  mobile_footer_layout->setContentsMargins(20, 8, 20, 14);
  mobile_footer_layout->addWidget(first_use_status);
  mobile_footer_layout->addWidget(first_use_close);
  first_use_page_layout->addWidget(mobile_footer);
#else
  auto *first_use_buttons = new QHBoxLayout;
  first_use_buttons->addStretch();
  first_use_buttons->addWidget(first_use_close);
  auto *first_use_content = new QWidget(first_use_page);
  auto *first_use_layout = new QVBoxLayout(first_use_content);
  first_use_layout->addStretch();
  first_use_layout->addWidget(first_use_title);
  first_use_layout->addWidget(first_use_intro);
  first_use_layout->addSpacing(8);
  first_use_layout->addLayout(connection_form);
  first_use_layout->addWidget(first_use_account_tabs);
  first_use_layout->addWidget(first_use_status);
  first_use_layout->addLayout(first_use_buttons);
  first_use_layout->addStretch();
  auto *first_use_scroll = new QScrollArea(first_use_page);
  first_use_scroll->setWidgetResizable(true);
  first_use_scroll->setFrameShape(QFrame::NoFrame);
  first_use_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  QScroller::grabGesture(first_use_scroll, QScroller::TouchGesture);
  first_use_scroll->setWidget(first_use_content);
  auto *first_use_page_layout = new QVBoxLayout(first_use_page);
  first_use_page_layout->setContentsMargins({});
  first_use_page_layout->addWidget(first_use_scroll);
#endif

  connection_label->setWordWrap(true);
  activity_label->setWordWrap(true);
  assignment_list->setHeaderLabels(
      {tr("Assignment"), tr("Map"), tr("Status"), tr("Due")});
  assignment_list->setRootIsDecorated(false);
  project_list->setHeaderLabels({tr("Venue / map"), tr("City / type"),
                                 tr("Status"), tr("Revision"), tr("Events")});
  project_list->setRootIsDecorated(true);
  event_list->setHeaderLabels({tr("Event"), tr("Date"), tr("Venue"), tr("Map"),
                               tr("Status"), tr("Series")});
  event_list->setRootIsDecorated(true);
#if defined(MAPPER_MOBILE)
  tabs->addTab(assignment_list, tr("My work"));
  tabs->addTab(project_list, tr("Maps"));
  tabs->addTab(event_list, tr("Events"));
  tabs->tabBar()->setExpanding(true);
  tabs->tabBar()->setUsesScrollButtons(false);
  for (auto *tree : {assignment_list, project_list, event_list}) {
    tree->setHeaderHidden(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->setWordWrap(true);
    tree->setUniformRowHeights(false);
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree->setStyleSheet(
        QStringLiteral("QTreeView::item { padding: 10px 6px; }"));
    QScroller::grabGesture(tree->viewport(), QScroller::TouchGesture);
    for (int column = 1; column < tree->columnCount(); ++column)
      tree->setColumnHidden(column, true);
  }
#else
  tabs->addTab(project_list, tr("Venues & maps"));
  tabs->addTab(event_list, tr("Events"));
  tabs->addTab(assignment_list, tr("My work"));
#endif
  auto *buttons = new QVBoxLayout;
#if defined(MAPPER_MOBILE)
  start_button->setText(tr("Open selected assignment"));
  open_project_button->setText(tr("View selected revision"));
  open_event_button->setText(tr("Open selected event"));
  new_button->setText(tr("Create connected map"));
  for (auto *button :
       {start_button, open_project_button, open_event_button, new_button}) {
    button->setMinimumHeight(44);
    button->setObjectName(QStringLiteral("mapHubLibraryPrimary"));
  }
  buttons->setSpacing(8);
#endif
  buttons->addWidget(start_button);
  buttons->addWidget(open_project_button);
  buttons->addWidget(open_event_button);
  buttons->addWidget(new_button);
#if defined(MAPPER_MOBILE)
  auto *close = new QPushButton(tr("Close"), this);
  refresh_button->setObjectName(QStringLiteral("mapHubLibraryQuiet"));
  close->setObjectName(QStringLiteral("mapHubLibraryQuiet"));
  auto *utility_buttons = new QHBoxLayout;
  utility_buttons->setSpacing(8);
  utility_buttons->addWidget(refresh_button);
  utility_buttons->addWidget(close);
  buttons->addLayout(utility_buttons);
#else
  buttons->addStretch();
  buttons->addWidget(refresh_button);
  auto *close = new QPushButton(tr("Close"), this);
  buttons->addWidget(close);
  buttons->setDirection(QBoxLayout::LeftToRight);
#endif
  auto *library_layout = new QVBoxLayout(library_page);
#if defined(MAPPER_MOBILE)
  library_layout->setContentsMargins(12, 12, 12, 12);
  auto *library_title = new QLabel(tr("Map Hub"), library_page);
  auto library_title_font = library_title->font();
  library_title_font.setPointSizeF(library_title_font.pointSizeF() * 1.6);
  library_title_font.setBold(true);
  library_title->setFont(library_title_font);
  library_layout->addWidget(library_title);
  connection_label->setStyleSheet(QStringLiteral("color: palette(mid);"));
  activity_label->setStyleSheet(QStringLiteral("color: palette(mid);"));
#endif
  library_layout->addWidget(connection_label);
  library_layout->addWidget(tabs, 1);
  library_layout->addWidget(activity_label);
  library_layout->addLayout(buttons);
  pages->addWidget(first_use_page);
  pages->addWidget(library_page);
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(pages);
  connect(first_use_browse, &QPushButton::clicked, this,
          &MapHubDialog::browseFirstUseWorkspace);
  connect(connect_button, &QPushButton::clicked, this,
          &MapHubDialog::connectExistingAccount);
  connect(invitation_button, &QPushButton::clicked, this,
          &MapHubDialog::openFirstUseInvitation);
  connect(first_use_close, &QPushButton::clicked, this, &QDialog::reject);
  connect(refresh_button, &QPushButton::clicked, this, &MapHubDialog::refresh);
  connect(start_button, &QPushButton::clicked, this,
          &MapHubDialog::startSelectedAssignment);
  connect(open_project_button, &QPushButton::clicked, this,
          &MapHubDialog::openSelectedProject);
  connect(open_event_button, &QPushButton::clicked, this,
          &MapHubDialog::openSelectedEvent);
  connect(new_button, &QPushButton::clicked, this,
          &MapHubDialog::createConnectedMap);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(assignment_list, &QTreeWidget::itemSelectionChanged, this,
          &MapHubDialog::updateActions);
  connect(project_list, &QTreeWidget::itemSelectionChanged, this,
          &MapHubDialog::updateActions);
  connect(event_list, &QTreeWidget::itemSelectionChanged, this,
          &MapHubDialog::updateActions);
  connect(tabs, &QTabWidget::currentChanged, this,
          &MapHubDialog::updateActions);
#if defined(MAPPER_MOBILE)
  connect(
      project_list, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem *item) {
        if (item->data(0, item_kind_role).toString() == QLatin1String("venue"))
          item->setExpanded(!item->isExpanded());
      });
  connect(event_list, &QTreeWidget::itemClicked, this,
          [](QTreeWidgetItem *item) {
            if (item->data(0, item_kind_role).toString() ==
                QLatin1String("event_group"))
              item->setExpanded(!item->isExpanded());
          });
#endif
  connect(assignment_list, &QTreeWidget::itemDoubleClicked, this,
          [this] { startSelectedAssignment(); });
  connect(project_list, &QTreeWidget::itemDoubleClicked, this,
          [this] { openSelectedProject(); });
  connect(event_list, &QTreeWidget::itemDoubleClicked, this,
          [this] { openSelectedEvent(); });
  refresh();
}

MapHubDialog::~MapHubDialog() = default;

void MapHubDialog::showFirstUse(const QString &problem) {
  clearPasskeyConnection();
  auto server =
      Settings::getInstance().getSetting(Settings::MapHub_ServerUrl).toString();
  if (server.trimmed().isEmpty())
    server = QStringLiteral("https://maps.zudark.net");
  first_use_server->setText(server);
  auto workspace_root = Settings::getInstance()
                            .getSetting(Settings::MapHub_WorkspaceRoot)
                            .toString();
  workspace_root = normalizedMapHubWorkspaceRoot(workspace_root);
  Settings::getInstance().setSetting(Settings::MapHub_WorkspaceRoot,
                                     workspace_root);
  first_use_workspace->setText(workspace_root);
  if (first_use_connection_summary) {
    first_use_connection_summary->setText(
        tr("%1\nMaps stay in %2").arg(server, workspace_root));
  }
  if (first_use_flow)
    first_use_flow->setCurrentIndex(0);
  first_use_status->setText(problem);
  pages->setCurrentWidget(first_use_page);
#if !defined(MAPPER_MOBILE)
  first_use_invite->setFocus();
#endif
}

void MapHubDialog::showLibrary() { pages->setCurrentWidget(library_page); }

void MapHubDialog::setFirstUseBusy(bool value, const QString &message) {
  first_use_status->setText(message);
  first_use_server->setEnabled(!value);
  first_use_workspace->setEnabled(!value);
  first_use_browse->setEnabled(!value);
  first_use_token->setEnabled(!value);
  first_use_invite->setEnabled(!value);
  first_use_account_tabs->setEnabled(!value);
  if (first_use_flow)
    first_use_flow->setEnabled(!value);
  connect_button->setEnabled(!value);
  invitation_button->setEnabled(!value);
}

void MapHubDialog::browseFirstUseWorkspace() {
  auto selected = QFileDialog::getExistingDirectory(
      this, tr("Choose Map Hub workspace folder"), first_use_workspace->text());
  if (!selected.isEmpty())
    first_use_workspace->setText(selected);
}

void MapHubDialog::beginPasskeyConnection() {
  QString server;
  QString workspace_root;
  if (!firstUseConnection(server, workspace_root))
    return;

  clearPasskeyConnection();
  setFirstUseBusy(true, tr("Creating a secure sign-in request…"));
  auto *request_client = new MapHubApiClient(server, {}, this);
  request_client->beginMapperConnection(
      tr("Mapper on iPhone"),
      [this, request_client, server, workspace_root](
          const QJsonObject &response, const MapHubApiClient::Error &error) {
        request_client->deleteLater();
        if (error) {
          setFirstUseBusy(
              false,
              error.http_status == 404
                  ? tr("Passkey sign-in is not available on this Map Hub "
                       "server yet. Use a connection token for now.")
                  : error.message);
          return;
        }

        const auto request_id =
            response.value(QStringLiteral("request_id")).toString();
        const auto device_secret =
            response.value(QStringLiteral("device_secret")).toString();
        const auto user_code =
            response.value(QStringLiteral("user_code")).toString();
        const auto verification_url =
            QUrl(response.value(QStringLiteral("verification_url")).toString());
        const auto server_url = QUrl::fromUserInput(server);
        const auto default_port = [](const QUrl &url) {
          return url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
        };
        const auto same_origin =
            verification_url.isValid() &&
            verification_url.scheme() == server_url.scheme() &&
            verification_url.host().compare(server_url.host(),
                                            Qt::CaseInsensitive) == 0 &&
            default_port(verification_url) == default_port(server_url) &&
            verification_url.userInfo().isEmpty();
        static const QRegularExpression code_pattern(
            QStringLiteral("^\\d{6}$"));
        if (QUuid(request_id).isNull() || device_secret.isEmpty() ||
            !code_pattern.match(user_code).hasMatch() || !same_origin) {
          setFirstUseBusy(
              false,
              tr("Map Hub returned an invalid passkey connection request."));
          return;
        }

        passkey_server = server;
        passkey_workspace_root = workspace_root;
        passkey_request_id = request_id;
        passkey_device_secret = device_secret;
        passkey_verification_url = verification_url;
        passkey_code->setText(user_code);
        passkey_status->setText(tr("Waiting for approval in Safari…"));
        passkey_open_browser->setText(tr("Open Map Hub in Safari"));
        setFirstUseBusy(false);
        first_use_flow->setCurrentWidget(passkey_page);
        passkey_timer->start();
        if (!QDesktopServices::openUrl(passkey_verification_url))
          passkey_status->setText(
              tr("Mapper could not open Safari. Tap below to try again."));
      });
}

void MapHubDialog::pollPasskeyConnection() {
  if (passkey_poll_in_flight || passkey_request_id.isEmpty() ||
      passkey_device_secret.isEmpty())
    return;

  passkey_poll_in_flight = true;
  const auto request_id = passkey_request_id;
  const auto device_secret = passkey_device_secret;
  auto *request_client = new MapHubApiClient(passkey_server, {}, this);
  request_client->exchangeMapperConnection(
      request_id, device_secret,
      [this, request_client, request_id](const QJsonObject &response,
                                         const MapHubApiClient::Error &error) {
        request_client->deleteLater();
        if (request_id != passkey_request_id)
          return;
        passkey_poll_in_flight = false;
        if (error) {
          if (error.http_status == 410 || error.http_status == 404) {
            passkey_timer->stop();
            passkey_status->setText(
                tr("This sign-in request expired. Go back and try again."));
          } else {
            passkey_status->setText(
                tr("Mapper is waiting for Map Hub: %1").arg(error.message));
          }
          return;
        }
        if (response.value(QStringLiteral("status")).toString() ==
            QLatin1String("pending")) {
          passkey_status->setText(tr("Waiting for approval in Safari…"));
          return;
        }
        const auto token = response.value(QStringLiteral("token")).toString();
        if (response.value(QStringLiteral("status")).toString() !=
                QLatin1String("connected") ||
            token.isEmpty()) {
          passkey_timer->stop();
          passkey_status->setText(
              tr("Map Hub returned an invalid sign-in response."));
          return;
        }

        QString storage_error;
        if (!saveFirstUseConnection(passkey_server, passkey_workspace_root,
                                    token, storage_error)) {
          passkey_timer->stop();
          passkey_status->setText(storage_error);
          return;
        }
        clearPasskeyConnection();
        refresh();
      });
}

void MapHubDialog::clearPasskeyConnection() {
  if (passkey_timer)
    passkey_timer->stop();
  passkey_poll_in_flight = false;
  passkey_server.clear();
  passkey_workspace_root.clear();
  passkey_request_id.clear();
  passkey_device_secret.clear();
  passkey_verification_url.clear();
  if (passkey_code)
    passkey_code->clear();
  if (passkey_status)
    passkey_status->clear();
}

bool MapHubDialog::firstUseConnection(QString &server,
                                      QString &workspace_root) {
  auto url = QUrl::fromUserInput(first_use_server->text().trimmed());
  if (!MapHubApiClient::isAcceptableServerUrl(url)) {
    QMessageBox::warning(this, tr("Map Hub"),
                         tr("Enter an HTTPS Map Hub URL. HTTP is allowed only "
                            "for localhost development."));
    return false;
  }
  server = url.adjusted(QUrl::StripTrailingSlash).toString();
  workspace_root = first_use_workspace->text().trimmed();
  workspace_root = normalizedMapHubWorkspaceRoot(workspace_root);
  if (DocumentPath::isContentUri(workspace_root)) {
    QMessageBox::warning(
        this, tr("Map Hub"),
        tr("Choose a local folder for Map Hub workspaces. Individual maps can "
           "still be exported through the document provider."));
    return false;
  }
  workspace_root = QDir(workspace_root).absolutePath();
  if (!QDir().mkpath(workspace_root) ||
      !QFileInfo(workspace_root).isWritable()) {
    QMessageBox::warning(
        this, tr("Map Hub"),
        tr("Mapper could not create or write to the local workspace folder."));
    return false;
  }
  return true;
}

bool MapHubDialog::saveFirstUseConnection(const QString &server,
                                          const QString &workspace_root,
                                          const QString &token,
                                          QString &error) {
  auto stored = MapHubCredentials::writeToken(server, token);
  if (!stored) {
    error = stored.error;
    return false;
  }
  auto old_server =
      Settings::getInstance().getSetting(Settings::MapHub_ServerUrl).toString();
  if (old_server != server)
    imagery::TileNetworkManager::instance().clearBearerCredential(
        QUrl(old_server));
  Settings::getInstance().setSetting(Settings::MapHub_ServerUrl, server);
  Settings::getInstance().setSetting(Settings::MapHub_WorkspaceRoot,
                                     workspace_root);
  imagery::TileNetworkManager::instance().setBearerCredential(
      QUrl(server), token.toUtf8(),
      MapHubCredentials::accountName(server).toUtf8());
  return true;
}

void MapHubDialog::connectExistingAccount() {
  QString server;
  QString workspace_root;
  if (!firstUseConnection(server, workspace_root))
    return;
  auto token = first_use_token->text().trimmed();
  if (token.isEmpty()) {
    QMessageBox::warning(this, tr("Map Hub"),
                         tr("Enter the account token you were given."));
    return;
  }
  setFirstUseBusy(true, tr("Checking the account…"));
  auto *request_client = new MapHubApiClient(server, token, this);
  request_client->library([this, request_client, server, workspace_root,
                           token](const QJsonObject &,
                                  const MapHubApiClient::Error &error) {
    request_client->deleteLater();
    if (error) {
      setFirstUseBusy(false, error.message);
      return;
    }
    QString storage_error;
    if (!saveFirstUseConnection(server, workspace_root, token, storage_error)) {
      setFirstUseBusy(false, storage_error);
      return;
    }
    first_use_token->clear();
    refresh();
  });
}

void MapHubDialog::openFirstUseInvitation() {
  QString server;
  QString workspace_root;
  if (!firstUseConnection(server, workspace_root))
    return;
  const auto invitation = first_use_invite->text().trimmed();
  static const QRegularExpression invitation_pattern(
      QStringLiteral("^[A-Za-z0-9_-]{20,200}$"));
  if (!invitation_pattern.match(invitation).hasMatch()) {
    QMessageBox::warning(this, tr("Map Hub"),
                         tr("Enter the invitation token from your map "
                            "librarian."));
    return;
  }
  auto url = QUrl::fromUserInput(server).adjusted(QUrl::StripTrailingSlash);
  url.setPath(QStringLiteral("/join/%1/").arg(invitation));
  url.setQuery(QString{});
  url.setFragment(QString{});
  if (!QDesktopServices::openUrl(url)) {
    QMessageBox::warning(this, tr("Map Hub"),
                         tr("Mapper could not open account setup in your "
                            "browser."));
    return;
  }
#if defined(MAPPER_MOBILE)
  first_use_flow->setCurrentIndex(2);
#else
  first_use_account_tabs->setCurrentIndex(1);
#endif
  first_use_status->setText(
      tr("Finish account setup in your browser, copy the Mapper connection "
         "token, then paste it here."));
  first_use_token->setFocus();
}

void MapHubDialog::setBusy(bool value, const QString &message) {
  busy = value;
  activity_label->setText(message);
  refresh_button->setEnabled(!busy);
  auto can_create = library_response.value(QStringLiteral("capabilities"))
                        .toObject()
                        .value(QStringLiteral("can_create_project"))
                        .toBool();
  new_button->setEnabled(
      !busy && can_create &&
      !library_response.value(QStringLiteral("venues")).toArray().isEmpty());
  updateActions();
}

void MapHubDialog::showError(const QString &title,
                             const MapHubApiClient::Error &error) {
  auto detail = error.message;
  if (!error.code.isEmpty())
    detail += tr("\n\nError: %1").arg(error.code);
  QMessageBox::warning(this, title, detail);
}

void MapHubDialog::refresh() {
  if (client) {
    client->deleteLater();
    client = nullptr;
  }
  auto server =
      Settings::getInstance().getSetting(Settings::MapHub_ServerUrl).toString();
  auto credential = MapHubCredentials::readToken(server);
  if (!credential.error.isEmpty()) {
    showFirstUse(tr("Mapper could not read the saved Map Hub account: %1")
                     .arg(credential.error));
    setFirstUseBusy(false, first_use_status->text());
    return;
  }
  if (credential.token.isEmpty()) {
    showFirstUse();
    setFirstUseBusy(false);
    return;
  }
  client = new MapHubApiClient(server, credential.token, this);
  if (!client->isConfigured()) {
    showFirstUse(client->configurationError());
    setFirstUseBusy(false, first_use_status->text());
    return;
  }
  showLibrary();
  imagery::TileNetworkManager::instance().setBearerCredential(
      QUrl(server), credential.token.toUtf8(),
      MapHubCredentials::accountName(server).toUtf8());
  setBusy(true, tr("Loading venues, maps, events, and assignments…"));
  auto *request_client = client.data();
  request_client->library(
      [this, server, request_client](const QJsonObject &response,
                                     const MapHubApiClient::Error &error) {
        if (client != request_client)
          return;
        if (error) {
          connection_label->setText(
              tr("Could not connect to %1: %2").arg(server, error.message));
          setBusy(false);
          return;
        }
        populate(response);
        setBusy(false);
      });
}

void MapHubDialog::populate(const QJsonObject &response) {
  library_response = response;
  auto organization = response.value(QStringLiteral("organization")).toObject();
  connection_label->setText(
      tr("Connected to %1")
          .arg(organization.value(QStringLiteral("name")).toString()));
  assignment_list->clear();
  for (const auto value :
       response.value(QStringLiteral("assignments")).toArray()) {
    auto object = value.toObject();
    auto *item = new QTreeWidgetItem({
        object.value(QStringLiteral("title")).toString(),
        projectTitle(object.value(QStringLiteral("project_id")).toString()),
        object.value(QStringLiteral("status")).toString(),
        object.value(QStringLiteral("due_on")).toString(),
    });
    item->setData(0, id_role, object.value(QStringLiteral("id")).toString());
    item->setData(0, project_id_role,
                  object.value(QStringLiteral("project_id")).toString());
    item->setData(0, status_role,
                  object.value(QStringLiteral("status")).toString());
    item->setData(0, package_type_role,
                  object.value(QStringLiteral("type")).toString());
    item->setData(0, title_role,
                  object.value(QStringLiteral("title")).toString());
#if defined(MAPPER_MOBILE)
    auto details =
        projectTitle(object.value(QStringLiteral("project_id")).toString());
    const auto status = object.value(QStringLiteral("status")).toString();
    const auto due = object.value(QStringLiteral("due_on")).toString();
    if (!status.isEmpty())
      details += QStringLiteral("  •  ") + status;
    if (!due.isEmpty())
      details += tr("  •  due %1").arg(due);
    item->setText(0, object.value(QStringLiteral("title")).toString() +
                         QLatin1Char('\n') + details);
#endif
    assignment_list->addTopLevelItem(item);
  }
  assignment_list->resizeColumnToContents(0);
  assignment_list->resizeColumnToContents(1);
  project_list->clear();
  QHash<QString, int> project_counts;
  QHash<QString, int> event_counts;
  for (const auto value :
       response.value(QStringLiteral("projects")).toArray()) {
    for (const auto venue :
         value.toObject().value(QStringLiteral("venues")).toArray())
      ++project_counts[venue.toObject().value(QStringLiteral("id")).toString()];
  }
  for (const auto value : response.value(QStringLiteral("events")).toArray()) {
    const auto venue_id =
        value.toObject().value(QStringLiteral("venue_id")).toString();
    if (!venue_id.isEmpty())
      ++event_counts[venue_id];
  }
  QHash<QString, QTreeWidgetItem *> venue_items;
  for (const auto value : response.value(QStringLiteral("venues")).toArray()) {
    auto venue = value.toObject();
    const auto venue_id = venue.value(QStringLiteral("id")).toString();
    auto *item = new QTreeWidgetItem({
        venue.value(QStringLiteral("name")).toString(),
        venue.value(QStringLiteral("city")).toString(),
        venue.value(QStringLiteral("status")).toString(),
        {},
        event_counts.value(venue_id) == 0
            ? tr("—")
            : tr("%n event(s)", nullptr, event_counts.value(venue_id)),
    });
    item->setData(0, item_kind_role, QStringLiteral("venue"));
    item->setToolTip(
        0, tr("%n map project(s)", nullptr, project_counts.value(venue_id)));
#if defined(MAPPER_MOBILE)
    auto venue_details = venue.value(QStringLiteral("city")).toString();
    const auto map_count =
        tr("%n map(s)", nullptr, project_counts.value(venue_id));
    if (!venue_details.isEmpty())
      venue_details += QStringLiteral("  •  ");
    venue_details += map_count;
    item->setText(0, venue.value(QStringLiteral("name")).toString() +
                         QLatin1Char('\n') + venue_details);
#endif
    project_list->addTopLevelItem(item);
    venue_items.insert(venue_id, item);
  }
  QTreeWidgetItem *unassigned = nullptr;
  for (const auto value :
       response.value(QStringLiteral("projects")).toArray()) {
    auto object = value.toObject();
    auto revision = object.value(QStringLiteral("current_revision")).toObject();
    auto add_project = [&](QTreeWidgetItem *parent) {
      auto *item = new QTreeWidgetItem({
          object.value(QStringLiteral("title")).toString(),
          object.value(QStringLiteral("kind")).toString(),
          object.value(QStringLiteral("status")).toString(),
          revision.isEmpty()
              ? tr("—")
              : tr("r%1").arg(revision.value(QStringLiteral("number")).toInt()),
          {},
      });
      item->setData(0, id_role, object.value(QStringLiteral("id")).toString());
      item->setData(0, item_kind_role, QStringLiteral("project"));
      item->setData(0, title_role,
                    object.value(QStringLiteral("title")).toString());
#if defined(MAPPER_MOBILE)
      QStringList details;
      details.append(object.value(QStringLiteral("kind")).toString());
      details.append(object.value(QStringLiteral("status")).toString());
      if (!revision.isEmpty())
        details.append(
            tr("r%1").arg(revision.value(QStringLiteral("number")).toInt()));
      details.removeAll(QString{});
      item->setText(0, object.value(QStringLiteral("title")).toString() +
                           QLatin1Char('\n') +
                           details.join(QStringLiteral("  •  ")));
#endif
      parent->addChild(item);
    };
    auto project_venues = object.value(QStringLiteral("venues")).toArray();
    if (project_venues.isEmpty()) {
      if (!unassigned) {
        unassigned =
            new QTreeWidgetItem({tr("No venue assigned"), {}, {}, {}, {}});
        unassigned->setData(0, item_kind_role, QStringLiteral("venue"));
        project_list->addTopLevelItem(unassigned);
      }
      add_project(unassigned);
    } else {
      for (const auto venue : project_venues) {
        auto *parent = venue_items.value(
            venue.toObject().value(QStringLiteral("id")).toString());
        if (parent)
          add_project(parent);
      }
    }
  }
  project_list->resizeColumnToContents(0);
  project_list->resizeColumnToContents(1);

  event_list->clear();
  QTreeWidgetItem *upcoming_events = nullptr;
  QTreeWidgetItem *earlier_events = nullptr;
  QTreeWidgetItem *undated_events = nullptr;
  auto event_group = [&](const QString &label,
                         QTreeWidgetItem *&group) -> QTreeWidgetItem * {
    if (!group) {
      group = new QTreeWidgetItem({label, {}, {}, {}, {}, {}});
      group->setData(0, item_kind_role, QStringLiteral("event_group"));
      event_list->addTopLevelItem(group);
    }
    return group;
  };
  const auto server =
      Settings::getInstance().getSetting(Settings::MapHub_ServerUrl).toString();
  for (const auto value : response.value(QStringLiteral("events")).toArray()) {
    auto object = value.toObject();
    const auto date_text =
        object.value(QStringLiteral("event_date")).toString();
    const auto date = QDate::fromString(date_text, Qt::ISODate);
    QTreeWidgetItem *parent = nullptr;
    if (!date.isValid())
      parent = event_group(tr("Date not set"), undated_events);
    else if (date >= QDate::currentDate())
      parent = event_group(tr("Upcoming events"), upcoming_events);
    else
      parent = event_group(tr("Earlier events"), earlier_events);
    auto *item = new QTreeWidgetItem({
        object.value(QStringLiteral("title")).toString(),
        date.isValid() ? QLocale().toString(date, QLocale::ShortFormat)
                       : tr("—"),
        object.value(QStringLiteral("venue_name")).toString(),
        object.value(QStringLiteral("map_project_title")).toString(),
        object.value(QStringLiteral("status")).toString(),
        object.value(QStringLiteral("series_name")).toString(),
    });
    item->setData(0, id_role, object.value(QStringLiteral("id")).toString());
    item->setData(0, item_kind_role, QStringLiteral("event"));
    item->setData(
        0, web_url_role,
        eventWebUrl(server, object.value(QStringLiteral("id")).toString()));
#if defined(MAPPER_MOBILE)
    QStringList event_details;
    event_details.append(date.isValid()
                             ? QLocale().toString(date, QLocale::ShortFormat)
                             : tr("Date not set"));
    event_details.append(object.value(QStringLiteral("venue_name")).toString());
    event_details.append(object.value(QStringLiteral("status")).toString());
    event_details.removeAll(QString{});
    QString text = object.value(QStringLiteral("title")).toString() +
                   QLatin1Char('\n') +
                   event_details.join(QStringLiteral("  •  "));
    const auto map_title =
        object.value(QStringLiteral("map_project_title")).toString();
    if (!map_title.isEmpty()) {
      text += QLatin1Char('\n');
      text += map_title;
    }
    item->setText(0, text);
#endif
    if (parent == earlier_events)
      parent->insertChild(0, item);
    else
      parent->addChild(item);
  }
  if (upcoming_events)
    upcoming_events->setExpanded(true);
  if (undated_events)
    undated_events->setExpanded(true);
  event_list->resizeColumnToContents(0);
  event_list->resizeColumnToContents(1);
  updateActions();
}

QString MapHubDialog::projectTitle(const QString &project_id) const {
  for (const auto value :
       library_response.value(QStringLiteral("projects")).toArray()) {
    auto project = value.toObject();
    if (project.value(QStringLiteral("id")).toString() == project_id)
      return project.value(QStringLiteral("title")).toString();
  }
  return tr("Map project");
}

void MapHubDialog::updateActions() {
  const auto current_tab = tabs->currentWidget();
  const auto showing_projects = current_tab == project_list;
  const auto showing_events = current_tab == event_list;
  const auto showing_assignments = current_tab == assignment_list;
  auto *assignment = assignment_list->currentItem();
  start_button->setVisible(showing_assignments);
  start_button->setEnabled(showing_assignments && !busy &&
                           assignmentCanStart(assignment));
  if (assignment && !MapHubApiClient::isMapperWorkspacePackageType(
                        assignment->data(0, package_type_role).toString())) {
    start_button->setToolTip(tr("Manage this assignment in Map Hub; it is not "
                                "a Mapper map workspace."));
  } else {
    start_button->setToolTip(
        assignment && !assignmentCanStart(assignment)
            ? tr("This assignment is no longer open for editing.")
            : tr("Open or resume the assignment's managed workspace."));
  }
  open_project_button->setVisible(showing_projects);
  open_project_button->setEnabled(
      showing_projects && !busy && project_list->currentItem() &&
      project_list->currentItem()->data(0, item_kind_role).toString() ==
          QLatin1String("project"));
  open_event_button->setVisible(showing_events);
  open_event_button->setEnabled(
      showing_events && !busy && event_list->currentItem() &&
      event_list->currentItem()->data(0, item_kind_role).toString() ==
          QLatin1String("event"));
  new_button->setVisible(showing_projects);
}

QString MapHubDialog::uniqueDestination(const QString &project_title,
                                        const QString &project_id,
                                        const QString &workspace_id,
                                        int revision_number,
                                        const QString &extension) const {
  auto root = Settings::getInstance()
                  .getSetting(Settings::MapHub_WorkspaceRoot)
                  .toString();
  root = normalizedMapHubWorkspaceRoot(root);
  auto project_directory = safeFileName(project_title) + QStringLiteral("--") +
                           shortStableId(project_id);
  auto directory = QDir(root).filePath(
      QDir(project_directory)
          .filePath(
              QStringLiteral("workspace-%1").arg(shortStableId(workspace_id))));
  QDir().mkpath(directory);
  auto base = safeFileName(project_title) +
              (revision_number > 0 ? QStringLiteral("-r%1").arg(revision_number)
                                   : QString{}) +
              QLatin1Char('.') + extension;
  auto path = QDir(directory).filePath(base);
  if (!QFileInfo::exists(path))
    return path;
  return QDir(directory).filePath(
      safeFileName(project_title) +
      QStringLiteral("-r%1-%2-%3.%4")
          .arg(revision_number)
          .arg(QDateTime::currentDateTimeUtc().toString(
              QStringLiteral("yyyyMMdd-HHmmss")))
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8),
               extension));
}

void MapHubDialog::openSelectedProject() {
  auto *item = project_list->currentItem();
  if (!item || busy ||
      item->data(0, item_kind_role).toString() != QLatin1String("project"))
    return;
  auto project_id = item->data(0, id_role).toString();
  auto title = item->data(0, title_role).toString();
  if (title.isEmpty())
    title = item->text(0);
  setBusy(true, tr("Loading current revision details for %1…").arg(title));
  client->projectManifest(
      project_id, [this, title](const QJsonObject &manifest,
                                const MapHubApiClient::Error &error) {
        if (error) {
          setBusy(false);
          showError(tr("Could not open library map"), error);
          return;
        }
        const auto revision =
            manifest.value(QStringLiteral("current_revision")).toObject();
        setBusy(false);
        if (revision.isEmpty()) {
          QMessageBox::information(
              this, tr("No approved revision"),
              tr("“%1” does not yet have an approved current revision.")
                  .arg(title));
          return;
        }

        const auto number = revision.value(QStringLiteral("number")).toInt();
        const auto kind =
            revision.value(QStringLiteral("artifact_kind")).toString();
        const auto name =
            revision.value(QStringLiteral("original_name")).toString();
        const auto sha = revision.value(QStringLiteral("sha256")).toString();
        QMessageBox::information(
            this, tr("Current approved revision"),
            tr("“%1” is currently at approved revision r%2.\n\n"
               "Artifact: %3%4\nSHA-256: %5\n\n"
               "This is an immutable library snapshot. To edit it, start an "
               "assigned work package so Mapper can checkpoint and submit "
               "every change through Map Hub.")
                .arg(title)
                .arg(number)
                .arg(kind.isEmpty() ? tr("map") : kind.toUpper())
                .arg(name.isEmpty() ? QString{} : tr(" — %1").arg(name))
                .arg(sha.isEmpty() ? tr("not supplied") : sha));
      });
}

void MapHubDialog::openSelectedEvent() {
  auto *item = event_list->currentItem();
  if (!item || busy ||
      item->data(0, item_kind_role).toString() != QLatin1String("event"))
    return;
  const auto url = QUrl(item->data(0, web_url_role).toString());
  if (!url.isValid() || !MapHubApiClient::isAcceptableServerUrl(url) ||
      !QDesktopServices::openUrl(url)) {
    QMessageBox::warning(this, tr("Could not open event"),
                         tr("Mapper could not open this event in Map Hub."));
  }
}

void MapHubDialog::startSelectedAssignment() {
  auto *item = assignment_list->currentItem();
  if (!item || busy)
    return;
  if (!assignmentCanStart(item)) {
    const auto package_type = item->data(0, package_type_role).toString();
    if (!MapHubApiClient::isMapperWorkspacePackageType(package_type)) {
      QMessageBox::information(
          this, tr("Managed in Map Hub"),
          tr("This assignment is tracked in Map Hub and does not create a "
             "Mapper map workspace."));
      return;
    }
    QMessageBox::information(
        this, tr("Assignment is not editable"),
        tr("This assignment is %1 and cannot be started or resumed.")
            .arg(item->data(0, status_role).toString()));
    return;
  }
  auto assignment_id = item->data(0, id_role).toString();
  auto project_id = item->data(0, project_id_role).toString();
  auto title = projectTitle(project_id);
  auto server =
      Settings::getInstance().getSetting(Settings::MapHub_ServerUrl).toString();
  auto manifest_url = projectManifestUrl(server, project_id);
  auto assignment_title = item->data(0, title_role).toString();
  if (assignment_title.isEmpty())
    assignment_title = item->text(0);
  setBusy(
      true,
      tr("Starting %1 and obtaining its editing lease…").arg(assignment_title));
  client->startAssignment(
      assignment_id,
      [this, assignment_id, project_id, title, manifest_url](
          const QJsonObject &response, const MapHubApiClient::Error &error) {
        if (error) {
          setBusy(false);
          showError(tr("Could not start assignment"), error);
          return;
        }
        setBusy(true, tr("Synchronizing project-authorized tiled sources…"));
        client->projectManifest(
            project_id, [this, response, assignment_id, title, manifest_url](
                            const QJsonObject &manifest,
                            const MapHubApiClient::Error &manifest_error) {
              ManagedMapWorkspace defaults;
              if (!manifest_error) {
                auto target =
                    manifest.value(QStringLiteral("target")).toObject();
                defaults.target_crs =
                    target.value(QStringLiteral("crs")).toString();
                defaults.target_scale =
                    target.value(QStringLiteral("scale")).toInt();
                defaults.symbol_standard =
                    target.value(QStringLiteral("symbol_standard")).toString();
                auto installed =
                    MapHubImageryCatalog::install(manifest, manifest_url);
                if (!installed)
                  QMessageBox::warning(this,
                                       tr("Map opened without project tiles"),
                                       installed.error);
              } else {
                QMessageBox::warning(
                    this, tr("Map opened without project metadata"),
                    tr("Mapper could not synchronize this project's target "
                       "settings or tiled sources: %1")
                        .arg(manifest_error.message));
              }
              beginWorkspace(response, assignment_id, title, defaults);
            });
      });
}

void MapHubDialog::beginWorkspace(const QJsonObject &response,
                                  const QString &assignment_id,
                                  const QString &project_title,
                                  const ManagedMapWorkspace &defaults) {
  auto workspace_object =
      response.value(QStringLiteral("workspace")).toObject();
  auto effective_revision =
      response.value(QStringLiteral("base_revision")).toObject();
  auto original_base =
      response.value(QStringLiteral("original_base_revision")).toObject();
  if (original_base.isEmpty())
    original_base = effective_revision;
  auto active_revision =
      response.value(QStringLiteral("active_revision")).toObject();
  auto lease = response.value(QStringLiteral("lease")).toObject();
  auto server =
      Settings::getInstance().getSetting(Settings::MapHub_ServerUrl).toString();
  auto workspace_id = workspace_object.value(QStringLiteral("id")).toString();
  auto project_id =
      workspace_object.value(QStringLiteral("project_id")).toString();
  auto work_package_id =
      workspace_object.value(QStringLiteral("work_package_id")).toString();
  if (QUuid(workspace_id).isNull() || QUuid(project_id).isNull() ||
      QUuid(work_package_id).isNull() || QUuid(assignment_id).isNull()) {
    setBusy(false);
    QMessageBox::warning(
        this, tr("Invalid workspace response"),
        tr("Map Hub did not return complete stable workspace identifiers. "
           "Nothing was downloaded or created locally."));
    return;
  }
  if (!lease.value(QStringLiteral("token")).toString().isEmpty()) {
    auto stored = MapHubCredentials::writeToken(
        MapHubCredentials::workspaceLeaseKey(server, workspace_id),
        lease.value(QStringLiteral("token")).toString());
    if (!stored) {
      setBusy(false);
      QMessageBox::warning(this, tr("Could not secure editing lease"),
                           stored.error);
      return;
    }
  }
  auto managed = defaults;
  managed.server_url = server;
  managed.organization_id =
      library_response.value(QStringLiteral("organization"))
          .toObject()
          .value(QStringLiteral("id"))
          .toString();
  managed.organization_name =
      library_response.value(QStringLiteral("organization"))
          .toObject()
          .value(QStringLiteral("name"))
          .toString();
  managed.project_id = project_id;
  managed.project_title = project_title;
  managed.work_package_id = work_package_id;
  managed.workspace_id = workspace_id;
  managed.assignment_id = assignment_id;
  managed.manifest_url = projectManifestUrl(server, managed.project_id);
  managed.status = workspace_object.value(QStringLiteral("status")).toString();
  managed.exclusive_editing =
      workspace_object.value(QStringLiteral("exclusive_editing")).toBool();
  managed.base_revision_id =
      original_base.value(QStringLiteral("id")).toString();
  managed.base_revision_number =
      original_base.value(QStringLiteral("number")).toInt();
  managed.base_sha256 =
      original_base.value(QStringLiteral("sha256")).toString();
  managed.active_revision_id =
      active_revision.value(QStringLiteral("id")).toString();
  managed.active_revision_number =
      active_revision.value(QStringLiteral("number")).toInt();
  managed.active_sha256 =
      active_revision.value(QStringLiteral("sha256")).toString();
  managed.base_artifact_kind =
      effective_revision.value(QStringLiteral("artifact_kind")).toString();
  managed.base_artifact_name =
      effective_revision.value(QStringLiteral("original_name")).toString();
  managed.lease_expires_at = QDateTime::fromString(
      lease.value(QStringLiteral("expires_at")).toString(), Qt::ISODate);
  auto download_url =
      QUrl(effective_revision.value(QStringLiteral("download_url")).toString());
  const auto baseline =
      MapHubApiClient::classifyWorkspaceBaseline(effective_revision);
  if (baseline == MapHubApiClient::WorkspaceBaseline::NoRevision) {
    setBusy(false);
    window->createConnectedMap(managed);
    accept();
    return;
  }
  if (baseline == MapHubApiClient::WorkspaceBaseline::IncompleteRevision) {
    setBusy(false);
    QMessageBox::warning(
        this, tr("Incomplete map baseline"),
        tr("Map Hub identified an existing base revision but did not provide "
           "a valid artifact download URL. Mapper did not create a blank map; "
           "refresh the assignment or ask the map librarian to repair the "
           "revision."));
    return;
  }
  static const QRegularExpression sha256_pattern(
      QStringLiteral("^[0-9a-fA-F]{64}$"));
  auto effective_sha =
      effective_revision.value(QStringLiteral("sha256")).toString();
  if (!sha256_pattern.match(effective_sha).hasMatch()) {
    setBusy(false);
    QMessageBox::warning(
        this, tr("Could not verify map"),
        tr("Map Hub returned downloadable map bytes without a valid SHA-256 "
           "checksum. Nothing was downloaded."));
    return;
  }
  auto extension = artifactExtension(effective_revision);
  if (managed.base_artifact_kind != QLatin1String("omap") &&
      managed.base_artifact_kind != QLatin1String("ocad") &&
      !managed.base_artifact_kind.isEmpty()) {
    setBusy(false);
    QMessageBox::warning(
        this, tr("Unsupported map baseline"),
        tr("Map Hub returned a %1 artifact where an .omap or OCAD map was "
           "required. Nothing was opened.")
            .arg(managed.base_artifact_kind));
    return;
  }
  auto destination = uniqueDestination(
      project_title, managed.project_id, managed.workspace_id,
      effective_revision.value(QStringLiteral("number")).toInt(), extension);
  managed.local_map_path = destination;
  setBusy(true,
          tr("Downloading and verifying r%1…")
              .arg(effective_revision.value(QStringLiteral("number")).toInt()));
  client->downloadArtifact(
      download_url, effective_sha, destination,
      [this, managed,
       effective_revision_number =
           effective_revision.value(QStringLiteral("number")).toInt()](
          const QString &path, const MapHubApiClient::Error &error) mutable {
        if (error) {
          setBusy(false);
          showError(tr("Could not download map"), error);
          return;
        }
        auto normalized_path = path;
        if (!path.endsWith(QLatin1String(".omap"), Qt::CaseInsensitive))
          normalized_path = uniqueDestination(
              managed.project_title, managed.project_id, managed.workspace_id,
              effective_revision_number, QStringLiteral("omap"));
        setBusy(false);
        if (window->openConnectedWorkspace(path, normalized_path, managed))
          accept();
      });
}

void MapHubDialog::createConnectedMap() {
  if (busy)
    return;
  ConnectedMapDialog dialog(library_response, this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  auto payload = dialog.payload();
  auto title = dialog.mapTitle();
  auto workspace_defaults = dialog.workspaceDefaults();
  auto start_locally = dialog.startLocally();
  auto assignee_name = dialog.assigneeName();
  auto payload_digest = QString::fromLatin1(
      QCryptographicHash::hash(
          QJsonDocument(payload).toJson(QJsonDocument::Compact),
          QCryptographicHash::Sha256)
          .toHex());
  QSettings transaction_state;
  transaction_state.beginGroup(QStringLiteral("MapHub/PendingProjectCreate"));
  auto idempotency_key =
      transaction_state.value(QStringLiteral("payload_digest")).toString() ==
              payload_digest
          ? transaction_state.value(QStringLiteral("idempotency_key"))
                .toString()
          : QString{};
  if (idempotency_key.isEmpty())
    idempotency_key =
        QStringLiteral("mapper-project-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  transaction_state.setValue(QStringLiteral("payload_digest"), payload_digest);
  transaction_state.setValue(QStringLiteral("idempotency_key"),
                             idempotency_key);
  transaction_state.endGroup();
  setBusy(true, tr("Creating the Map Hub project before the local map…"));
  client->createProject(
      payload, idempotency_key,
      [this, title, workspace_defaults, start_locally, assignee_name](
          const QJsonObject &response, const MapHubApiClient::Error &error) {
        if (error) {
          setBusy(false);
          showError(tr("Could not create connected map"), error);
          return;
        }
        QSettings transaction_state;
        transaction_state.remove(QStringLiteral("MapHub/PendingProjectCreate"));
        if (!start_locally) {
          setBusy(false);
          QMessageBox::information(
              this, tr("Connected map assigned"),
              tr("“%1” and its first work package were created in Map Hub "
                 "and assigned to %2. Their Mapper account will see it in My "
                 "work; no local map was created on this computer.")
                  .arg(title, assignee_name));
          refresh();
          return;
        }
        auto assignment_id =
            response.value(QStringLiteral("assignment_id")).toString();
        if (assignment_id.isEmpty()) {
          setBusy(false);
          QMessageBox::warning(
              this, tr("Project created, but local work could not start"),
              tr("The project is safe in Map Hub, but the server did not "
                 "return its assignment ID. Refresh the library before opening "
                 "it."));
          return;
        }
        setBusy(true, tr("Project created. Starting its managed workspace…"));
        client->startAssignment(
            assignment_id, [this, assignment_id, title, workspace_defaults](
                               const QJsonObject &started,
                               const MapHubApiClient::Error &start_error) {
              if (start_error) {
                setBusy(false);
                showError(
                    tr("Project created, but its workspace could not start"),
                    start_error);
                return;
              }
              beginWorkspace(started, assignment_id, title, workspace_defaults);
            });
      });
}

} // namespace OpenOrienteering
