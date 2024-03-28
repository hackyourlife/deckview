#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <DeckLinkAPI.h>

#include "deckview.h"

void list_devices(void)
{
	IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
	if(!iterator) {
		printf("A DeckLink iterator could not be created. The DeckLink drivers may not be installed.\n");
		return;
	}

	IDeckLink* device = NULL;
	unsigned int id = 0;

	while(iterator->Next(&device) == S_OK) {
		const char* model;
		const char* name;
		device->GetModelName(&model);
		device->GetDisplayName(&name);
		printf("Device %d: %s (%s)\n", id, name, model);
		free((void*) name);
		free((void*) model);
		id++;
	}

	if(iterator) {
		iterator->Release();
	}

	if(id == 0) {
		printf("No Desktop Video devices found.\n");
	}
}

IDeckLink* get_device(const char* name)
{
	IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
	if(!iterator) {
		printf("A DeckLink iterator could not be created. The DeckLink drivers may not be installed.\n");
		return NULL;
	}

	IDeckLink* device = NULL;

	while(iterator->Next(&device) == S_OK) {
		const char* display_name;
		device->GetDisplayName(&display_name);
		if(!strcmp(name, display_name)) {
			free((void*) display_name);
			iterator->Release();
			return device;
		}
		free((void*) display_name);
	}

	if(iterator) {
		iterator->Release();
	}

	return NULL;
}

int main(int argc, char** argv)
{
	if(argc != 2) {
		list_devices();
		return 0;
	}

	const char* name = argv[1];

	IDeckLink* device = get_device(name);

	if(!device) {
		printf("Device not found\n");
		return 1;
	}

	if(GXInit(device)) {
		GXMain();
		GXDestroy();
	}

	device->Release();

	printf("Bye\n");

	return 0;
}
