// Port settings that live in the game's own options, so they are chosen in
// its menus and kept in its save. Compiled with pmd-red's include path.

#include "global.h"
#include "game_options.h"
#include "../core/port.h"

// Others > Game Options > Screen (outside dungeons). Stored in the bit the DS
// version uses for its touch screen option, which the GBA game never reads.
bool Port_WantsBottomScreen(void)
{
    return gGameOptionsRef != NULL && gGameOptionsRef->touchScreen;
}
