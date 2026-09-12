#include "gui/launcherGui/LauncherGui.h"
#include "gui/windows/MainWindow.h"
#include <wx/msgdlg.h>

IMPLEMENT_APP(Sph::App);

NAMESPACE_SPH_BEGIN

bool App::OnInit() {
#ifndef SPH_DEBUG
    wxDisableAsserts();
#endif

    this->Connect(MAIN_LOOP_TYPE, MainLoopEventHandler(App::processEvents));

    try {
        if (wxTheApp->argc > 1) {
            Path path(String(wxTheApp->argv[1].wc_str()));
            window = new MainWindow(path);
        } else {
            window = new MainWindow();
        }
        window->SetAutoLayout(true);
        window->Show();
        return true;
    } catch (const std::exception& e) {
        wxMessageBox(e.what(), "OpenSPH Error", wxOK | wxICON_ERROR);
        return false;
    } catch (...) {
        wxMessageBox("Unknown initialization error", "OpenSPH Error", wxOK | wxICON_ERROR);
        return false;
    }
}

int App::OnExit() {
    return 0;
}

NAMESPACE_SPH_END
