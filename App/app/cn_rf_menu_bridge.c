#include "app/cn_rf_ops.h"

#include "ui/menu.h"

void __wrap_MENU_ProcessKeys(KEY_Code_t key, bool pressed, bool held)
{
    if (CN_RF_OPS_HandleMenuKey(key, pressed, held))
        return;
    __real_MENU_ProcessKeys(key, pressed, held);
}

void __wrap_UI_DisplayMenu(void)
{
    if (CN_RF_OPS_IsCustomMenuPage()) {
        CN_RF_OPS_DrawMenuPage();
        return;
    }
    __real_UI_DisplayMenu();
}
