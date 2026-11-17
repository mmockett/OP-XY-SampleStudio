// Sample Studio: a sample editor in the OP-XY's sample library. The editor (editor.cpp) takes the
// library's keys and screen through the SDK's sample_studio sites; the player (player.cpp) plays
// the edit on the audio board, after the master EQ. Both use only opxy/sample_studio.hpp.
#include <opxy/extension.hpp>
#include <opxy/sample_studio.hpp>
#include "editor.cpp"
#include "player.cpp"

namespace opxy { namespace extensions { namespace sample_studio {
// No events: the library's page is the whole extension.
extern constexpr Hooks hooks = Hooks();
extern const studio::Editor editor = {edit::press, edit::release_key, edit::turn_encoder, edit::frame};
extern const studio::Player player = {play::receive, play::mixer_begin, play::voice, play::gain, play::level, play::master};
}}}  // namespace opxy::extensions::sample_studio
