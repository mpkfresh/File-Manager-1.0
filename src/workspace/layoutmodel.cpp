#include "layoutmodel.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <algorithm>
#include <utility>

namespace filemanager::workspace {
namespace {

void setError(QString* output, const QString& message)
{
    if (output)
        *output = message;
}

QString directionName(SplitDirection direction)
{
    return direction == SplitDirection::Horizontal ? "horizontal" : "vertical";
}

} // namespace

LayoutNode LayoutNode::panelNode(PanelState state)
{
    while (state.viewModes.size() < state.tabPaths.size())
        state.viewModes.append(0);
    while (state.showHidden.size() < state.tabPaths.size())
        state.showHidden.append(false);
    while (state.folderPlacements.size() < state.tabPaths.size())
        state.folderPlacements.append(0);
    while (state.showFileExtensions.size() < state.tabPaths.size())
        state.showFileExtensions.append(true);
    while (state.groupings.size() < state.tabPaths.size())
        state.groupings.append(0);
    while (state.selectedPaths.size() < state.tabPaths.size())
        state.selectedPaths.append(QStringList{});
    while (state.focusedPaths.size() < state.tabPaths.size())
        state.focusedPaths.append(QString{});
    while (state.expandedPaths.size() < state.tabPaths.size())
        state.expandedPaths.append(QStringList{});
    while (state.verticalScrolls.size() < state.tabPaths.size())
        state.verticalScrolls.append(0);
    while (state.horizontalScrolls.size() < state.tabPaths.size())
        state.horizontalScrolls.append(0);
    LayoutNode node;
    node.type = NodeType::Panel;
    node.panel = std::move(state);
    return node;
}

LayoutNode LayoutNode::splitNode(
    SplitDirection splitDirection, QList<LayoutNode> splitChildren, QList<int> splitSizes)
{
    LayoutNode node;
    node.type = NodeType::Split;
    node.direction = splitDirection;
    node.children = std::move(splitChildren);
    node.sizes = std::move(splitSizes);
    return node;
}

bool LayoutNode::isValid(QString* error) const
{
    if (type == NodeType::Panel) {
        if (panel.tabPaths.isEmpty()) {
            setError(error, "A panel must contain at least one tab.");
            return false;
        }
        if (panel.currentTab < 0 || panel.currentTab >= panel.tabPaths.size()) {
            setError(error, "The current tab index is outside the panel tab list.");
            return false;
        }
        for (const QString& path : panel.tabPaths) {
            if (path.trimmed().isEmpty()) {
                setError(error, "Panel tab paths cannot be empty.");
                return false;
            }
        }
        if ((!panel.viewModes.isEmpty() && panel.viewModes.size() != panel.tabPaths.size())
            || (!panel.showHidden.isEmpty() && panel.showHidden.size() != panel.tabPaths.size())
            || (!panel.folderPlacements.isEmpty()
                && panel.folderPlacements.size() != panel.tabPaths.size())
            || (!panel.showFileExtensions.isEmpty()
                && panel.showFileExtensions.size() != panel.tabPaths.size())
            || (!panel.groupings.isEmpty() && panel.groupings.size() != panel.tabPaths.size())
            || (!panel.selectedPaths.isEmpty()
                && panel.selectedPaths.size() != panel.tabPaths.size())
            || (!panel.focusedPaths.isEmpty()
                && panel.focusedPaths.size() != panel.tabPaths.size())
            || (!panel.expandedPaths.isEmpty()
                && panel.expandedPaths.size() != panel.tabPaths.size())
            || (!panel.verticalScrolls.isEmpty()
                && panel.verticalScrolls.size() != panel.tabPaths.size())
            || (!panel.horizontalScrolls.isEmpty()
                && panel.horizontalScrolls.size() != panel.tabPaths.size())) {
            setError(error, "Per-tab view state must match the panel tab list.");
            return false;
        }
        for (int mode : panel.viewModes) {
            if (mode < 0 || mode > 5) {
                setError(error, "A tab has an invalid view mode.");
                return false;
            }
        }
        for (int placement : panel.folderPlacements) {
            if (placement < 0 || placement > 2) {
                setError(error, "A tab has an invalid folder placement.");
                return false;
            }
        }
        for (int grouping : panel.groupings) {
            if (grouping < 0 || grouping > 4) {
                setError(error, "A tab has an invalid group-by mode.");
                return false;
            }
        }
        for (const QStringList& paths : panel.selectedPaths) {
            if (paths.size() > 256) {
                setError(error, "A tab has too many selected paths to restore.");
                return false;
            }
        }
        for (const QStringList& paths : panel.expandedPaths) {
            if (paths.size() > 128) {
                setError(error, "A tab has too many expanded paths to restore.");
                return false;
            }
        }
        if (std::any_of(panel.verticalScrolls.cbegin(), panel.verticalScrolls.cend(),
                [](int value) { return value < 0; })
                || std::any_of(panel.horizontalScrolls.cbegin(), panel.horizontalScrolls.cend(),
                    [](int value) { return value < 0; })) {
            setError(error, "A tab has an invalid saved scroll position.");
            return false;
        }
        return true;
    }

    if (children.size() < 2) {
        setError(error, "A split must contain at least two children.");
        return false;
    }
    if (!sizes.isEmpty() && sizes.size() != children.size()) {
        setError(error, "Split sizes must match the number of children.");
        return false;
    }
    for (const LayoutNode& child : children) {
        if (!child.isValid(error))
            return false;
    }
    return true;
}

QJsonObject LayoutNode::toJson() const
{
    QJsonObject json;
    json["schemaVersion"] = CurrentSchemaVersion;
    json["type"] = type == NodeType::Panel ? "panel" : "split";
    if (type == NodeType::Panel) {
        QJsonArray tabs;
        for (const QString& path : panel.tabPaths)
            tabs.append(QDir::fromNativeSeparators(path));
        json["tabs"] = tabs;
        json["currentTab"] = panel.currentTab;
        QJsonArray viewModes;
        QJsonArray hiddenStates;
        QJsonArray folderPlacements;
        QJsonArray extensionStates;
        QJsonArray groupings;
        QJsonArray selectedPaths;
        QJsonArray focusedPaths;
        QJsonArray expandedPaths;
        QJsonArray verticalScrolls;
        QJsonArray horizontalScrolls;
        for (int index = 0; index < panel.tabPaths.size(); ++index) {
            viewModes.append(panel.viewModes.value(index, 0));
            hiddenStates.append(panel.showHidden.value(index, false));
            folderPlacements.append(panel.folderPlacements.value(index, 0));
            extensionStates.append(panel.showFileExtensions.value(index, true));
            groupings.append(panel.groupings.value(index, 0));
            QJsonArray selected;
            for (const QString& path : panel.selectedPaths.value(index))
                selected.append(QDir::fromNativeSeparators(path));
            selectedPaths.append(selected);
            focusedPaths.append(QDir::fromNativeSeparators(panel.focusedPaths.value(index)));
            QJsonArray expanded;
            for (const QString& path : panel.expandedPaths.value(index))
                expanded.append(QDir::fromNativeSeparators(path));
            expandedPaths.append(expanded);
            verticalScrolls.append(panel.verticalScrolls.value(index, 0));
            horizontalScrolls.append(panel.horizontalScrolls.value(index, 0));
        }
        json["viewModes"] = viewModes;
        json["showHidden"] = hiddenStates;
        json["folderPlacements"] = folderPlacements;
        json["showFileExtensions"] = extensionStates;
        json["groupings"] = groupings;
        json["selectedPaths"] = selectedPaths;
        json["focusedPaths"] = focusedPaths;
        json["expandedPaths"] = expandedPaths;
        json["verticalScrolls"] = verticalScrolls;
        json["horizontalScrolls"] = horizontalScrolls;
        return json;
    }

    json["direction"] = directionName(direction);
    QJsonArray childArray;
    for (const LayoutNode& child : children)
        childArray.append(child.toJson());
    json["children"] = childArray;
    QJsonArray sizeArray;
    for (int size : sizes)
        sizeArray.append(size);
    json["sizes"] = sizeArray;
    return json;
}

std::optional<LayoutNode> LayoutNode::fromJson(const QJsonObject& json, QString* error)
{
    const int schemaVersion = json.value("schemaVersion").toInt();
    if (schemaVersion < 1 || schemaVersion > CurrentSchemaVersion) {
        setError(error, "Unsupported workspace layout schema version.");
        return std::nullopt;
    }

    const QString typeName = json.value("type").toString();
    LayoutNode node;
    if (typeName == "panel") {
        node.type = NodeType::Panel;
        for (const QJsonValue& value : json.value("tabs").toArray())
            node.panel.tabPaths.append(QDir::cleanPath(value.toString()));
        node.panel.currentTab = json.value("currentTab").toInt(-1);
        if (schemaVersion >= 2) {
            for (const QJsonValue& value : json.value("viewModes").toArray())
                node.panel.viewModes.append(value.toInt());
            for (const QJsonValue& value : json.value("showHidden").toArray())
                node.panel.showHidden.append(value.toBool());
        } else {
            while (node.panel.viewModes.size() < node.panel.tabPaths.size())
                node.panel.viewModes.append(0);
            while (node.panel.showHidden.size() < node.panel.tabPaths.size())
                node.panel.showHidden.append(false);
        }
        if (schemaVersion >= 3) {
            for (const QJsonValue& value : json.value("folderPlacements").toArray())
                node.panel.folderPlacements.append(value.toInt());
        } else {
            while (node.panel.folderPlacements.size() < node.panel.tabPaths.size())
                node.panel.folderPlacements.append(0);
        }
        if (schemaVersion >= 4) {
            for (const QJsonValue& value : json.value("showFileExtensions").toArray())
                node.panel.showFileExtensions.append(value.toBool(true));
        } else {
            while (node.panel.showFileExtensions.size() < node.panel.tabPaths.size())
                node.panel.showFileExtensions.append(true);
        }
        if (schemaVersion >= 6) {
            for (const QJsonValue& value : json.value("groupings").toArray())
                node.panel.groupings.append(value.toInt());
        } else {
            while (node.panel.groupings.size() < node.panel.tabPaths.size())
                node.panel.groupings.append(0);
        }
        if (schemaVersion >= 7) {
            for (const QJsonValue& tab : json.value("selectedPaths").toArray()) {
                QStringList paths;
                for (const QJsonValue& value : tab.toArray())
                    paths.append(QDir::cleanPath(value.toString()));
                node.panel.selectedPaths.append(paths);
            }
            for (const QJsonValue& value : json.value("focusedPaths").toArray())
                node.panel.focusedPaths.append(QDir::cleanPath(value.toString()));
            for (const QJsonValue& tab : json.value("expandedPaths").toArray()) {
                QStringList paths;
                for (const QJsonValue& value : tab.toArray())
                    paths.append(QDir::cleanPath(value.toString()));
                node.panel.expandedPaths.append(paths);
            }
            for (const QJsonValue& value : json.value("verticalScrolls").toArray())
                node.panel.verticalScrolls.append(value.toInt());
            for (const QJsonValue& value : json.value("horizontalScrolls").toArray())
                node.panel.horizontalScrolls.append(value.toInt());
        } else {
            while (node.panel.selectedPaths.size() < node.panel.tabPaths.size())
                node.panel.selectedPaths.append(QStringList{});
            while (node.panel.focusedPaths.size() < node.panel.tabPaths.size())
                node.panel.focusedPaths.append(QString{});
            while (node.panel.expandedPaths.size() < node.panel.tabPaths.size())
                node.panel.expandedPaths.append(QStringList{});
            while (node.panel.verticalScrolls.size() < node.panel.tabPaths.size())
                node.panel.verticalScrolls.append(0);
            while (node.panel.horizontalScrolls.size() < node.panel.tabPaths.size())
                node.panel.horizontalScrolls.append(0);
        }
    } else if (typeName == "split") {
        node.type = NodeType::Split;
        const QString direction = json.value("direction").toString();
        if (direction != "horizontal" && direction != "vertical") {
            setError(error, "A split has an invalid direction.");
            return std::nullopt;
        }
        node.direction = direction == "horizontal"
            ? SplitDirection::Horizontal
            : SplitDirection::Vertical;
        for (const QJsonValue& value : json.value("children").toArray()) {
            auto child = fromJson(value.toObject(), error);
            if (!child)
                return std::nullopt;
            node.children.append(std::move(*child));
        }
        for (const QJsonValue& value : json.value("sizes").toArray())
            node.sizes.append(value.toInt());
    } else {
        setError(error, "A workspace node has an invalid type.");
        return std::nullopt;
    }

    if (!node.isValid(error))
        return std::nullopt;
    return node;
}

} // namespace filemanager::workspace
