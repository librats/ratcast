#include "people_controller.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QSysInfo>

#include <filesystem>
#include <utility>

#include "call_controller.h"
#include "ratcast/client/known_servers.h"
#include "ratcast/client/rats_url.h"

#include "librats/util/logger.h"

namespace ratcast::app {
namespace {

QString user_name() {
    QString name = qEnvironmentVariable("USER");
    if (name.isEmpty()) name = qEnvironmentVariable("USERNAME");  // Windows
    return name;
}

std::optional<PersonId> person_of(const QString& hex) {
    return PersonId::from_hex(hex.toStdString());
}

QString hex_of(const PersonId& person) { return QString::fromStdString(person.to_hex()); }

}  // namespace

PeopleController::Environment PeopleController::Environment::system() {
    Environment environment;
    const std::string data = data_folder().toStdString();
    environment.people.data_dir = data;
    environment.people.built_in_servers = KnownServers::built_in();
    environment.people.known_servers_file =
        (std::filesystem::path(data) / KnownServers::kFile).string();
    if (const std::optional<librats::HostEndpoint> server = parse_server(RATCAST_DEFAULT_SERVER))
        environment.people.servers.push_back(*server);
    environment.people.device_name = QSysInfo::machineHostName().toStdString();
    // The name the window kept before people were known (CallController's).
    environment.name = QSettings().value(QStringLiteral("name"), user_name()).toString();
    return environment;
}

PeopleController::PeopleController(QObject* parent)
    : QObject(parent), environment_(Environment::system()) {}

PeopleController::~PeopleController() {
    ++generation_;
    people_.reset();
}

void PeopleController::setEnvironment(Environment environment) {
    environment_ = std::move(environment);
    start();
}

void PeopleController::componentComplete() {
    if (!people_) start();
}

void PeopleController::start() {
    const uint64_t generation = ++generation_;
    people_.reset();
    PeopleConfig config = environment_.people;
    config.name = environment_.name.toStdString();
    PeopleHandlers handlers{
        .changed = [this, generation] { post(generation, [this] { Q_EMIT changed(); }); },
        .device_asks =
            [this, generation](const std::string& device) {
                post(generation, [this, device = QString::fromStdString(device)] {
                    device_asking_ = device.isEmpty() ? tr("A device") : device;
                    Q_EMIT deviceChanged();
                });
            },
        .joined =
            [this, generation](bool joined) {
                post(generation, [this, joined] {
                    joining_ = false;
                    Q_EMIT deviceChanged();
                    set_notice(joined ? tr("This device is %1 now.").arg(name())
                                      : tr("The other device said no."));
                    Q_EMIT this->joined(joined);
                });
            },
    };
    Result<std::unique_ptr<People>> started = People::start(std::move(config), std::move(handlers));
    if (!started) {
        LOG_ERROR("app", "people not started: " << to_string(started.error()));
        set_notice(started.error() == Errc::Malformed
                       ? tr("Your key in the data folder could not be read.")
                       : tr("Friends are not reachable: the network did not start."));
    } else {
        people_ = std::move(*started);
    }
    Q_EMIT changed();
    Q_EMIT deviceChanged();
}

void PeopleController::post(uint64_t generation, std::function<void()> task) {
    QMetaObject::invokeMethod(
        this,
        [this, generation, task = std::move(task)] {
            if (generation == generation_) task();
        },
        Qt::QueuedConnection);
}

void PeopleController::set_notice(const QString& notice) {
    if (notice == notice_) return;
    notice_ = notice;
    Q_EMIT noticeChanged();
}

QString PeopleController::name() const {
    return people_ ? QString::fromStdString(people_->name()) : environment_.name;
}

void PeopleController::setName(const QString& name) {
    if (name == this->name()) return;
    environment_.name = name;
    if (people_) people_->set_name(name.toStdString());
    Q_EMIT changed();
}

QString PeopleController::tag() const {
    return people_ ? QString::fromStdString(people_->person().tag()) : QString();
}

QString PeopleController::person() const { return people_ ? hex_of(people_->person()) : QString(); }

QString PeopleController::link() const {
    return people_ ? QString::fromStdString(people_->link()) : QString();
}

QVariantList PeopleController::friends() const {
    QVariantList list;
    if (!people_) return list;
    for (const Friend& f : people_->friends()) {
        list.push_back(QVariantMap{
            {QStringLiteral("person"), hex_of(f.person)},
            {QStringLiteral("name"), QString::fromStdString(f.name)},
            {QStringLiteral("tag"), QString::fromStdString(f.person.tag())},
        });
    }
    return list;
}

QVariantList PeopleController::requests() const {
    QVariantList list;
    if (!people_) return list;
    for (const FriendRequest& r : people_->requests()) {
        list.push_back(QVariantMap{
            {QStringLiteral("person"), hex_of(r.person)},
            {QStringLiteral("name"), QString::fromStdString(r.name)},
            {QStringLiteral("tag"), QString::fromStdString(r.person.tag())},
            {QStringLiteral("incoming"), r.incoming},
            {QStringLiteral("declined"), r.declined},
        });
    }
    return list;
}

int PeopleController::asking() const {
    if (!people_) return 0;
    int count = 0;
    for (const FriendRequest& r : people_->requests()) count += r.incoming ? 1 : 0;
    return count;
}

bool PeopleController::isPersonLink(const QString& link) {
    return parse_person_link(link.trimmed().toStdString()).has_value();
}

bool PeopleController::isDeviceLink(const QString& link) {
    const std::optional<PersonLink> parsed = parse_person_link(link.trimmed().toStdString());
    return parsed && parsed->kind == PersonLink::Kind::Device;
}

bool PeopleController::add(const QString& link) {
    if (!people_) return false;
    const std::string text = link.trimmed().toStdString();
    const std::optional<PersonLink> parsed = parse_person_link(text);
    if (!parsed || parsed->kind != PersonLink::Kind::Friend) {
        set_notice(tr("That is not a link to a person."));
        return false;
    }
    if (parsed->person == people_->person()) {
        set_notice(tr("That is your own link."));
        return false;
    }
    if (people_->is_friend(parsed->person)) {
        set_notice(tr("You are friends already."));
        return false;
    }
    if (!people_->add(text)) return false;
    set_notice(tr("Asked. They become your friend once they accept."));
    return true;
}

void PeopleController::accept(const QString& person) {
    if (const std::optional<PersonId> id = person_of(person); id && people_) people_->accept(*id);
}

void PeopleController::decline(const QString& person) {
    if (const std::optional<PersonId> id = person_of(person); id && people_) people_->decline(*id);
}

void PeopleController::remove(const QString& person) {
    if (const std::optional<PersonId> id = person_of(person); id && people_) people_->remove(*id);
}

void PeopleController::resetLink() {
    if (!people_) return;
    people_->reset_link();
    set_notice(tr("Your link is new: the old one no longer works."));
}

void PeopleController::copy(const QString& text) { QGuiApplication::clipboard()->setText(text); }

void PeopleController::makeDeviceLink() {
    if (!people_) return;
    device_link_ = QString::fromStdString(people_->device_link());
    device_asking_.clear();
    Q_EMIT deviceChanged();
}

void PeopleController::cancelDeviceLink() {
    if (!people_) return;
    people_->cancel_device_link();
    device_link_.clear();
    device_asking_.clear();
    Q_EMIT deviceChanged();
}

void PeopleController::allowDevice(bool allowed) {
    if (!people_) return;
    people_->allow_device(allowed);
    device_link_.clear();
    device_asking_.clear();
    Q_EMIT deviceChanged();
    if (allowed) set_notice(tr("The device is you now."));
}

bool PeopleController::joinAsDevice(const QString& link) {
    if (!people_) return false;
    const std::string text = link.trimmed().toStdString();
    const std::optional<PersonLink> parsed = parse_person_link(text);
    if (!parsed || parsed->kind != PersonLink::Kind::Device) {
        set_notice(tr("That is not a link to add a device."));
        return false;
    }
    if (parsed->person == people_->person()) {
        set_notice(tr("That link is this device's own."));
        return false;
    }
    if (!people_->join_as_device(text)) return false;
    joining_ = true;
    Q_EMIT deviceChanged();
    set_notice(tr("Waiting for the other device to say yes…"));
    return true;
}

bool PeopleController::saveBackup(const QUrl& file) {
    if (!people_) return false;
    QFile out(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        set_notice(tr("The backup could not be written there."));
        return false;
    }
    out.write(QByteArray::fromStdString(people_->backup()));
    out.close();
    set_notice(tr("Backed up. Keep the file to yourself: it is you."));
    return true;
}

bool PeopleController::restoreBackup(const QUrl& file) {
    if (!people_) return false;
    QFile in(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!in.open(QIODevice::ReadOnly)) {
        set_notice(tr("The backup could not be read."));
        return false;
    }
    const QByteArray text = in.read(1 << 20);
    if (!people_->restore(text.toStdString())) {
        set_notice(tr("That file is no backup of ratcast's."));
        return false;
    }
    set_notice(tr("Restored: this device is %1 again.").arg(name()));
    return true;
}

}  // namespace ratcast::app
