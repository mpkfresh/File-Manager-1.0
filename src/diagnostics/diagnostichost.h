#pragma once

namespace filemanager::diagnostics {
// Private, bounded stdin/stdout protocol used by the application's storage owner.
int runDiagnosticHost();
}
