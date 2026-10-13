#include "OnboardingController.h"

#include "Registry.h"

namespace {

constexpr const char *kThemeKey = "theme.name";

slint::SharedString ss(const std::string &s) { return slint::SharedString(std::string_view(s)); }

} // namespace

OnboardingController::OnboardingController(slint::ComponentHandle<AppWindow> ui, SaveService &save)
    : ui_(std::move(ui)), save_(save)
{
    const auto &st = ui_->global<AppState>();
    st.on_onboarding_theme([this](int index) { pickTheme(index); });
    st.on_finish_onboarding([this] { finish(); });
    st.on_dismiss_onboarding([this] { skip(); });

    save_.onResult([this](const SaveService::Result &result) {
        if (!finishing_)
            return;
        finishing_ = false;
        const auto &st = ui_->global<AppState>();
        st.set_onboarding_saving(false);
        if (result.ok || result.needsUserConfig) {
            // Saved (the footer says so), or the personal-config prompt needs the screen.
            staged_ = false;
            close();
            return;
        }
        st.set_onboarding_error(ss(result.message));
    });
}

void OnboardingController::pickTheme(int index)
{
    if (index < 0 || index > 4 || save_.saving())
        return;
    const auto &app = ui_->global<AppState>();
    app.set_onboarding_error("");
    staged_ = true;
    // The Appearance page's path: validation, and choosing the saved value again
    // drops the pending change instead of staging a no-op.
    ui_->global<AppearanceState>().invoke_set_index(ui_->global<K>().get_name(), index);
}

void OnboardingController::finish()
{
    if (finishing_)
        return;
    const auto &app = ui_->global<AppState>();
    if (!staged_ || !save_.pending().find(kThemeKey)) {
        close();
        return;
    }
    app.set_onboarding_error("");
    app.set_onboarding_saving(true);
    finishing_ = true;
    auto status = app.get_status();
    if (status.config_source == "install") {
        // Never written to: make the personal copy first (shell.exe -config:init),
        // which then saves there. Step 3's text says so.
        ui_->global<SaveState>().invoke_create_user_config();
    } else {
        save_.save();
    }
    // A save already running (Ctrl+S just before): its result ends this one too.
}

void OnboardingController::skip()
{
    if (finishing_)
        return;
    if (staged_ && save_.pending().find(kThemeKey))
        save_.forget(kThemeKey);
    staged_ = false;
    close();
}

void OnboardingController::close()
{
    const auto &st = ui_->global<AppState>();
    st.set_onboarding_shown(false);
    st.set_onboarding_error("");
    staged_ = false;
    registry::app::set(L"OnboardingDismissed", 1);
    registry::app::set(L"ClassicMenu", st.get_classic_menu_option() ? 1 : 0);
}
