#pragma once

// The first-run dialog (ui/pages/onboarding.slint). Steps 1 and 2 run
// AppState.action("config-init" / "register-user") in AppController. Step 3
// stages theme.name through the Appearance page's own path (AppearanceState
// set-index, so the cards show the effective value the same way), and Finish
// saves it with SaveService: `shell.exe -config:check` first, like every save.
// Skip and Esc close without saving and drop a theme staged here.

#include "app.h"

#include "SaveService.h"

class OnboardingController
{
public:
    OnboardingController(slint::ComponentHandle<AppWindow> ui, SaveService &save);

private:
    void pickTheme(int index);
    void finish();
    void skip();
    void close();

    slint::ComponentHandle<AppWindow> ui_;
    SaveService &save_;
    bool staged_ = false;    // this dialog put theme.name in the pending changes
    bool finishing_ = false; // Finish is waiting for the save
};
