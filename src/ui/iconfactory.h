#pragma once

#include <QColor>
#include <QIcon>

namespace filemanager::ui {

enum class Symbol { Folder, Search, ContentSearch, Command, NewFolder, File, Rename, Batch, Undo, Copy, Move,
    Trash, Open, Inspector, Split, Transfers };

QIcon symbolIcon(Symbol symbol, const QColor& color = QColor("#c7cad4"));

} // namespace filemanager::ui
