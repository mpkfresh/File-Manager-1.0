#pragma once

#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <optional>

namespace filemanager::workspace {

enum class NodeType { Panel, Split };
enum class SplitDirection { Horizontal, Vertical };

struct PanelState
{
    QStringList tabPaths;
    int currentTab = 0;
    QList<int> viewModes;
    QList<bool> showHidden;
    QList<int> folderPlacements;
    QList<bool> showFileExtensions;
    QList<int> groupings;
    QList<QStringList> selectedPaths;
    QStringList focusedPaths;
    QList<QStringList> expandedPaths;
    QList<int> verticalScrolls;
    QList<int> horizontalScrolls;

    bool operator==(const PanelState&) const = default;
};

struct LayoutNode
{
    static constexpr int CurrentSchemaVersion = 7;

    NodeType type = NodeType::Panel;
    PanelState panel;
    SplitDirection direction = SplitDirection::Horizontal;
    QList<LayoutNode> children;
    QList<int> sizes;

    static LayoutNode panelNode(PanelState state);
    static LayoutNode splitNode(
        SplitDirection direction, QList<LayoutNode> children, QList<int> sizes = {});

    bool isValid(QString* error = nullptr) const;
    QJsonObject toJson() const;
    static std::optional<LayoutNode> fromJson(const QJsonObject& json, QString* error = nullptr);

    bool operator==(const LayoutNode&) const = default;
};

} // namespace filemanager::workspace
