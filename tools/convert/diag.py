import os
from mwfiles import DATA_FILES, Archives, load_db

print('DATA_FILES:', DATA_FILES)
print('Morrowind.esm exists:', (DATA_FILES / 'Morrowind.esm').exists())
print('Morrowind.bsa exists:', (DATA_FILES / 'Morrowind.bsa').exists())

arch = Archives()
print('BSAs loaded:', len(arch.bsas))
if arch.bsas:
    print('first BSA path:', arch.bsas[0].path)
    print('files in first BSA:', len(arch.bsas[0].files))
    names = arch.names()
    print('total files in archives:', len(names))
    print('meshes: %d  textures: %d' % (
        len([n for n in names if n.startswith('meshes\\')]),
        len([n for n in names if n.startswith('textures\\')])))
    sample = [n for n in names if 'base_anim' in n][:5]
    print('base_anim files:', sample)

print('exists meshes\\base_anim.nif:', arch.exists(r'meshes\base_anim.nif'))

print('loading db...')
db = load_db()
print('cells:', len(db['CELL']))
print('objects:', len(db['objects']))
print('scripts:', len(db['SCPT']))
print('races:', len(db['RACE']))
