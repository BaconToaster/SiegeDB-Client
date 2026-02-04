#include <cstdio>

#include "siegedb/siegedb.hh"

int main() {
    auto siegedb = siegedb::SiegeDB::New("API_URL", "TOKEN");
    if (!siegedb) {
        printf("Failed to initialize SiegeDB\n");
        return 1;
    }

    if (!siegedb->Attach("RainbowSix.exe")) {
        printf("Failed to attach to process\n");
        return 1;
    }

    // specific classes/fields:
    /*auto offsets = siegedb->GetOffsets(
        "classes=Entity{m_BoundingVolume,m_Components},BoundingVolume{m_Min,m_"
        "Max}");*/
    // whole dump:
    auto offsets = siegedb->GetOffsets();
    if (offsets.is_null()) {
        printf("Failed to get offsets\n");
        return 1;
    }

    printf("Offsets: %s\n", offsets.dump(4).c_str());
    return 0;
}
