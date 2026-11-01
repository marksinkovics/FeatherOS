#include "support/terminal_device.h"

namespace
{

TestTerminalDevice* Owner(void* context)
{
    return static_cast<TestTerminalDevice*>(context);
}

} // namespace

TestTerminalDevice::TestTerminalDevice()
    : device{
          .context = this,
          .write = [](void* context, const char* text, size_t length)
          {
              Owner(context)->output.append(text, length);
              Owner(context)->output_count += length;
          },
          .write_output = nullptr,
          .erase_character = [](void* context) { Owner(context)->output += "<BS>"; },
          .set_cursor_visible = [](void* context, bool visible)
          { Owner(context)->cursor_visible = visible; },
          .reset_modes = [](void* context) { ++Owner(context)->resets; },
          .output_count = [](void* context) { return Owner(context)->output_count; },
          .start_fresh_row = [](void* context) { ++Owner(context)->fresh_rows; },
          .poll =
              [](void* context)
          {
              if (Owner(context)->on_poll)
              {
                  Owner(context)->on_poll();
              }
          },
      }
{
}
