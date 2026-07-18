/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_dialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "collaboration/managed_map_workspace.h"
#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_credentials.h"
#include "collaboration/map_hub_imagery_catalog.h"
#include "gui/main_window.h"
#include "imagery/tile_network_manager.h"
#include "settings.h"

namespace OpenOrienteering {

namespace {

constexpr int id_role = Qt::UserRole;
constexpr int project_id_role = Qt::UserRole + 1;
constexpr int status_role = Qt::UserRole + 2;
constexpr int package_type_role = Qt::UserRole + 3;

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
  url.setQuery({});
  url.setFragment({});
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
    resize(720, 780);
    auto *form_widget = new QWidget(this);
    auto *form = new QFormLayout(form_widget);
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
    crs->setPlaceholderText(QStringLiteral("EPSG:6596"));
    form->addRow(tr("Target CRS:"), crs);
    scale->setRange(100, 1000000);
    scale->setValue(10000);
    scale->setSingleStep(500);
    form->addRow(tr("Map scale:"), scale);
    standard->setPlaceholderText(QStringLiteral("ISOM 2017-2"));
    form->addRow(tr("Symbol standard:"), standard);
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
    exclusive->setChecked(true);
    form->addRow(exclusive);
    auto *note =
        new QLabel(tr("The Map Hub project and audit record are created before "
                      "Mapper creates the local .omap workspace."),
                   this);
    note->setWordWrap(true);
    form->addRow(note);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
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
      connection_label(new QLabel(this)), activity_label(new QLabel(this)),
      tabs(new QTabWidget(this)), assignment_list(new QTreeWidget(this)),
      project_list(new QTreeWidget(this)),
      start_button(new QPushButton(tr("Start selected assignment"), this)),
      open_project_button(
          new QPushButton(tr("Current revision details…"), this)),
      new_button(new QPushButton(tr("New connected map…"), this)),
      refresh_button(new QPushButton(tr("Refresh"), this)) {
  setWindowTitle(tr("Map Hub — library and assignments"));
  resize(880, 580);
  connection_label->setWordWrap(true);
  activity_label->setWordWrap(true);
  assignment_list->setHeaderLabels(
      {tr("Assignment"), tr("Map"), tr("Status"), tr("Due")});
  assignment_list->setRootIsDecorated(false);
  project_list->setHeaderLabels(
      {tr("Map"), tr("Venue"), tr("Status"), tr("Revision")});
  project_list->setRootIsDecorated(false);
  tabs->addTab(assignment_list, tr("My work"));
  tabs->addTab(project_list, tr("Map library"));
  auto *buttons = new QHBoxLayout;
  buttons->addWidget(start_button);
  buttons->addWidget(open_project_button);
  buttons->addWidget(new_button);
  buttons->addStretch();
  buttons->addWidget(refresh_button);
  auto *close = new QPushButton(tr("Close"), this);
  buttons->addWidget(close);
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(connection_label);
  layout->addWidget(tabs, 1);
  layout->addWidget(activity_label);
  layout->addLayout(buttons);
  connect(refresh_button, &QPushButton::clicked, this, &MapHubDialog::refresh);
  connect(start_button, &QPushButton::clicked, this,
          &MapHubDialog::startSelectedAssignment);
  connect(open_project_button, &QPushButton::clicked, this,
          &MapHubDialog::openSelectedProject);
  connect(new_button, &QPushButton::clicked, this,
          &MapHubDialog::createConnectedMap);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(assignment_list, &QTreeWidget::itemSelectionChanged, this,
          &MapHubDialog::updateActions);
  connect(project_list, &QTreeWidget::itemSelectionChanged, this,
          &MapHubDialog::updateActions);
  connect(assignment_list, &QTreeWidget::itemDoubleClicked, this,
          [this] { startSelectedAssignment(); });
  connect(project_list, &QTreeWidget::itemDoubleClicked, this,
          [this] { openSelectedProject(); });
  refresh();
}

MapHubDialog::~MapHubDialog() = default;

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
  if (client)
    client->deleteLater();
  auto server =
      Settings::getInstance().getSetting(Settings::MapHub_ServerUrl).toString();
  auto credential = MapHubCredentials::readToken(server);
  if (!credential.error.isEmpty()) {
    connection_label->setText(
        tr("Credential error: %1 Open Settings → Map Hub to reconnect.")
            .arg(credential.error));
    setBusy(false);
    return;
  }
  client = new MapHubApiClient(server, credential.token, this);
  if (!client->isConfigured()) {
    connection_label->setText(client->configurationError() + QLatin1Char(' ') +
                              tr("Open Settings → Map Hub to connect."));
    setBusy(false);
    return;
  }
  imagery::TileNetworkManager::instance().setBearerCredential(
      QUrl(server), credential.token.toUtf8(),
      MapHubCredentials::accountName(server).toUtf8());
  setBusy(true, tr("Loading the latest library and assignments…"));
  client->library([this, server](const QJsonObject &response,
                                 const MapHubApiClient::Error &error) {
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
    assignment_list->addTopLevelItem(item);
  }
  assignment_list->resizeColumnToContents(0);
  assignment_list->resizeColumnToContents(1);
  project_list->clear();
  for (const auto value :
       response.value(QStringLiteral("projects")).toArray()) {
    auto object = value.toObject();
    QStringList venues;
    for (const auto venue :
         object.value(QStringLiteral("venue_names")).toArray())
      venues.append(venue.toString());
    auto revision = object.value(QStringLiteral("current_revision")).toObject();
    auto *item = new QTreeWidgetItem({
        object.value(QStringLiteral("title")).toString(),
        venues.join(QStringLiteral(", ")),
        object.value(QStringLiteral("status")).toString(),
        revision.isEmpty()
            ? tr("—")
            : tr("r%1").arg(revision.value(QStringLiteral("number")).toInt()),
    });
    item->setData(0, id_role, object.value(QStringLiteral("id")).toString());
    project_list->addTopLevelItem(item);
  }
  project_list->resizeColumnToContents(0);
  project_list->resizeColumnToContents(1);
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
  auto *assignment = assignment_list->currentItem();
  start_button->setEnabled(!busy && assignmentCanStart(assignment));
  if (assignment &&
      !MapHubApiClient::isMapperWorkspacePackageType(
          assignment->data(0, package_type_role).toString())) {
    start_button->setToolTip(
        tr("Manage this assignment in Map Hub; it is not a Mapper map workspace."));
  } else {
    start_button->setToolTip(
        assignment && !assignmentCanStart(assignment)
            ? tr("This assignment is no longer open for editing.")
            : tr("Open or resume the assignment's managed workspace."));
  }
  open_project_button->setEnabled(!busy && project_list->currentItem());
}

QString MapHubDialog::uniqueDestination(const QString &project_title,
                                        const QString &project_id,
                                        const QString &workspace_id,
                                        int revision_number,
                                        const QString &extension) const {
  auto root = Settings::getInstance()
                  .getSetting(Settings::MapHub_WorkspaceRoot)
                  .toString();
  if (root.isEmpty()) {
#ifdef Q_OS_ANDROID
    root =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("map-hub-workspaces"));
#else
    root = QDir(QStandardPaths::writableLocation(
                    QStandardPaths::DocumentsLocation))
               .filePath(tr("Mapper Workspaces"));
#endif
  }
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
  if (!item || busy)
    return;
  auto project_id = item->data(0, id_role).toString();
  auto title = item->text(0);
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
  setBusy(
      true,
      tr("Starting %1 and obtaining its editing lease…").arg(item->text(0)));
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
  if (!download_url.isValid() || download_url.isEmpty()) {
    setBusy(false);
    window->createConnectedMap(managed);
    accept();
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
