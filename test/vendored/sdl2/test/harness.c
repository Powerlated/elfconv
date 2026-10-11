/* Minimal SDL2 testautomation harness for the vendored Pixels and Surface suites.
 * The suite functions and assertions remain upstream SDL2 test code.
 */
#include "SDL.h"
#include "SDL_test.h"

extern SDLTest_TestSuiteReference pixelsTestSuite;
extern SDLTest_TestSuiteReference surfaceTestSuite;

static const SDLTest_TestSuiteReference *test_suites[] = {
    &pixelsTestSuite,
    &surfaceTestSuite,
    NULL
};

int main(void)
{
    int result;

    if (SDL_Init(0) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    result = SDLTest_RunSuites(test_suites, NULL, 0, NULL, 1);
    SDL_Quit();
    return result;
}
