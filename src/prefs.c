// prefs.c

#include <SDL.h>

#include "main.h"
#include "prefs.h"
#include "music.h"
#include "soundfx.h"
#include "hiscore.h"
#include "keyselect.h"

typedef struct Preference
{
    const char*   keyName;
    void*         valuePtr;
    unsigned int  valueLength;
} Preference;

Preference prefList[] =
{
    { "MusicOn",            &musicOn,           sizeof(MBoolean         ) },
    { "SoundOn",            &soundOn,           sizeof(MBoolean         ) },
    { "KeyBindings",        playerKeys,         sizeof(playerKeys       ) },
    { "HighScores",         scores,             sizeof(scores           ) },
    { "BestCombo",          &best,              sizeof(best             ) },
    { "Fullscreen",         &fullscreen,        sizeof(fullscreen       ) },
    { "Widescreen",         &widescreen,        sizeof(widescreen       ) },
    { "CrispUpscaling",     &crispUpscaling,    sizeof(crispUpscaling   ) },
};

static SDL_RWops* GetPrefsStream(const char* openmode)
{
	static char path[1024];
	char* userDir = SDL_GetPrefPath(NULL, "CandyCrisis");
	SDL_snprintf(path, sizeof(path), "%sCandyCrisisPrefs.bin", userDir);
	SDL_free(userDir);
	return SDL_RWFromFile(path, openmode);
}

void LoadPrefs()
{
	SDL_RWops* stream = GetPrefsStream("rb");
	if (!stream)
	{
		return;
	}

	for (int i = 0; i < arrsize(prefList); i++)
	{
		Preference* pref = &prefList[i];

		SDL_RWseek(stream, 0, RW_SEEK_SET);

		while (1)
		{
			size_t numRead;
			Uint8 keyLength;
			char key[256];
			unsigned int contentsLength;

			numRead = SDL_RWread(stream, &keyLength, sizeof(keyLength), 1);
			if (!numRead)
				break;
			SDL_RWread(stream, key, keyLength, 1);
			key[keyLength] = '\0';
			SDL_RWread(stream, &contentsLength, sizeof(contentsLength), 1);

			if (!SDL_strncmp(key, pref->keyName, SDL_strlen(pref->keyName)))
			{
				if (contentsLength != pref->valueLength)
					break;
				SDL_RWread(stream, pref->valuePtr, pref->valueLength, 1);
				break;
			}
			else
			{
				SDL_RWseek(stream, contentsLength, RW_SEEK_CUR);
			}
		}
	}

	SDL_RWclose(stream);

#ifdef __amigaos__
	// The file is written and read in this machine's byte order (a PC's
	// file has every length the wrong way round, so nothing in it is
	// taken). Even so, what is read is not trusted further than this:
	// keys index the key state array, names are printed as strings.
	for (int p = 0; p < 2; p++)
		for (int k = 0; k < 4; k++)
			if (playerKeys[p][k] <= SDLK_UNKNOWN || playerKeys[p][k] >= SDLK_LAST)
				SDL_memcpy(playerKeys, defaultPlayerKeys, sizeof(playerKeys));
	for (int i = 0; i < arrsize(scores); i++)
		scores[i].name[kNameLength-1] = '\0';
	best.name[kNameLength-1] = '\0';
	for (int x = 0; x < kGridAcross; x++)
		for (int y = 0; y < kGridDown; y++)
			if (best.grid[x][y] < kEmpty || best.grid[x][y] > kGray)
				best.grid[x][y] = kEmpty;
#endif
}

void SavePrefs()
{
	SDL_RWops* stream = GetPrefsStream("wb");
	if (!stream)
	{
		return;
	}

	for (int i = 0; i < arrsize(prefList); i++)
	{
		const Preference* pref = &prefList[i];

		Uint8 keyLength = SDL_strlen(pref->keyName);

		SDL_RWwrite(stream, &keyLength, sizeof(keyLength), 1);
		SDL_RWwrite(stream, pref->keyName, SDL_strlen(pref->keyName), 1);
		SDL_RWwrite(stream, &pref->valueLength, sizeof(pref->valueLength), 1);
		SDL_RWwrite(stream, pref->valuePtr, pref->valueLength, 1);
	}

	SDL_RWclose(stream);
}
