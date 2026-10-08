#include "bridge.h"
#include "buttons.h"

void app_main(void)
{
    buttons_init();
    bridge_init();
    bridge_start();
}
