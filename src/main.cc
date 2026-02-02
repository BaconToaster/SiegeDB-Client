#include <cstdio>

#include "siegedb/siegedb.hh"

int main() {
    auto siegedb = siegedb::SiegeDB::New(
        "http://localhost:3000/api",
        "cfc94a0627246f25858d6e7f7e81a8534c11fae3d7d82989605f60d5ddf78f6c");
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
