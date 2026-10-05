import csv
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
CJSON=Path(os.environ.get("IDF_PATH",""))/"components/json/cJSON"


class PartitionTests(unittest.TestCase):
    def test_vnext_layout_preserves_low_regions_and_fills_16mib(self):
        def rows(name):
            return {r[0].strip():r for r in csv.reader((ROOT/name).read_text().splitlines()) if r and not r[0].startswith("#")}
        old,new=rows("partitions.csv"),rows("partitions-vnext.csv")
        for key in ("nvs","phy_init","otadata","coredump"):self.assertEqual(old[key],new[key])
        ordered=sorted((int(r[3],0),int(r[4],0)) for r in new.values())
        for (offset,size),(following,_) in zip(ordered,ordered[1:]):self.assertLessEqual(offset+size,following)
        self.assertEqual(sum(ordered[-1]),0x1000000)
        self.assertEqual(int(new["ota_0"][4],0),0x400000)
        self.assertEqual(int(new["ota_1"][4],0),0x400000)


@unittest.skipUnless((CJSON/"cJSON.c").exists(),"Set IDF_PATH for cJSON")
class AssetStoreTests(unittest.TestCase):
    def test_native_flash_simulation(self):
        with tempfile.TemporaryDirectory(prefix="pet-asset-store-") as directory:
            binary=str(Path(directory)/"store");obj=str(Path(directory)/"json.o");cc=os.environ.get("CC","cc")
            subprocess.run([cc,"-std=c11","-Wno-deprecated-declarations","-fsanitize=address,undefined",
                "-c",str(CJSON/"cJSON.c"),"-o",obj],check=True)
            sources=[ROOT/"main"/name for name in ("pet_asset_store.c","pet_journal.c","pet_install.c","pet_control_wire.c")]
            subprocess.run([cc,"-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
                "-I",str(ROOT/"tests/onboarding_host/include"),"-I",str(CJSON),"-I",str(ROOT/"main"),
                *map(str,sources),str(ROOT/"tests/asset_store_test.c"),obj,"-o",binary],check=True)
            subprocess.run([binary],check=True)


if __name__=="__main__":unittest.main()
