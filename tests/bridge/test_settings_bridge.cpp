// The SettingsBridge of the baseline: the interface language is kept in the store, a language that is not offered is refused, and the state of
// the user's own certificate authority is passed through for the Settings page.
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "pbc/store.h"
#include "settings_bridge.h"
#include "test_support.h"
#include "tmp_dir.h"

namespace {

int g_argc = 1;
char g_arg0[] = "test_settings_bridge";
char* g_argv[] = {g_arg0, nullptr};
QCoreApplication g_app(g_argc, g_argv);

pbc::SystemClock g_clock;

std::unique_ptr<pbc::Store> open_store(const pbtest::TmpDir& d) {
    auto r = pbc::Store::open(d.file("data"), g_clock);
    if (!r.ok()) PB_FAIL_AT("Store::open failed: " + r.error.key);
    return std::move(r.value);
}

}  // namespace

PB_TEST(the_language_is_automatic_until_the_user_chooses_one) {
    pbtest::TmpDir d;
    auto store = open_store(d);
    SettingsBridge::Config c;
    c.store = store.get();
    SettingsBridge settings(c);
    PB_CHECK_EQ(settings.language().toStdString(), std::string("auto"));
    PB_CHECK_EQ(settings.languageChoices().size(), 3);
}

PB_TEST(a_chosen_language_is_saved_announced_and_survives_a_reopen) {
    pbtest::TmpDir d;
    {
        auto store = open_store(d);
        SettingsBridge::Config c;
        c.store = store.get();
        SettingsBridge settings(c);
        QSignalSpy changed(&settings, &SettingsBridge::languageChanged);
        PB_CHECK(settings.setLanguage(QStringLiteral("de")));
        PB_CHECK_EQ(changed.count(), 1);
        PB_CHECK_EQ(settings.language().toStdString(), std::string("de"));
        PB_CHECK(settings.errorKey().isEmpty());
    }
    auto store = open_store(d);
    SettingsBridge::Config c;
    c.store = store.get();
    SettingsBridge settings(c);
    PB_CHECK_EQ(settings.language().toStdString(), std::string("de"));
}

PB_TEST(a_language_that_is_not_offered_is_refused_and_changes_nothing) {
    pbtest::TmpDir d;
    auto store = open_store(d);
    SettingsBridge::Config c;
    c.store = store.get();
    SettingsBridge settings(c);
    QSignalSpy changed(&settings, &SettingsBridge::languageChanged);
    PB_CHECK(!settings.setLanguage(QStringLiteral("fr")));
    PB_CHECK_EQ(changed.count(), 0);
    PB_CHECK_EQ(settings.language().toStdString(), std::string("auto"));
}

PB_TEST(the_state_of_the_own_certificate_authority_is_passed_through) {
    pbtest::TmpDir d;
    auto store = open_store(d);
    SettingsBridge::Config c;
    c.store = store.get();
    c.custom_ca_status = QStringLiteral("loaded");
    c.custom_ca_count = 2;
    SettingsBridge settings(c);
    PB_CHECK_EQ(settings.customCaStatus().toStdString(), std::string("loaded"));
    PB_CHECK_EQ(settings.customCaCount(), 2);
}

PB_TEST_MAIN()
