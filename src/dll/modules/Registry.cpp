#include "dll/module/ModuleManager.hpp"

#include "dll/modules/client/ClientModules.hpp"
#include "dll/modules/hud/HudModules.hpp"
#include "dll/modules/movement/MovementModules.hpp"
#include "dll/modules/render/RenderModules.hpp"
#include "dll/ui/ClickGui.hpp"
#include "dll/ui/HudEditor.hpp"
#include "dll/ui/Notifications.hpp"
#include "dll/ui/Onboarding.hpp"

namespace velyx {

void registerBuiltInModules(ModuleManager& manager) {

    manager.add<ClickGui>();
    manager.add<HudEditor>();
    manager.add<Notifications>();
    manager.add<Onboarding>();

    registerHudModules(manager);
    registerClientModules(manager);
    registerMovementModules(manager);
    registerRenderModules(manager);
}

}
