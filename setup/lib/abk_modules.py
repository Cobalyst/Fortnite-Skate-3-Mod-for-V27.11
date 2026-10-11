"""Maps a Skate 3 ABKC sound bank's modules -> players -> sample indices (the same order
vgmstream numbers its subsongs, i.e. our raw/<bank>_<n>.wav files). Layout from vgmstream
ea_eaac_abk.c (modules table at 0x1C, 0x3C-byte module entries + 4 bytes per player).
python abk_modules.py <bank.abk> [--raw]"""
import struct, sys

def parse(path):
    d = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    u16 = lambda o: struct.unpack_from(">H", d, o)[0]
    num_modules = u16(0x0A)
    table = u32(0x1C)
    modules = []
    sound_no = 0
    seen = {}
    for m in range(num_modules):
        num_players = d[table + 0x24]
        num_classes = d[table + 0x27]
        module_data = u32(table + 0x2C)
        entry = {"index": m, "at": table, "header": d[table:table + 0x3C], "players": []}
        for j in range(num_players):
            player_offset = u32(table + 0x3C + 4 * j)
            st = u32(module_data + player_offset + 0x04)
            p = {"offset": module_data + player_offset, "samples_table": st, "sounds": []}
            if st in seen:
                p["sounds"] = seen[st]
                p["dupe"] = True
            else:
                n = u32(st)
                for k in range(n):
                    e = st + 4 + 12 * k
                    idx = u16(e)
                    if idx == 0xFFFF:
                        continue
                    p["sounds"].append((sound_no, idx, d[e + 2], d[e + 3]))
                    sound_no += 1
                seen[st] = p["sounds"]
            entry["players"].append(p)
        entry["classes"] = num_classes
        modules.append(entry)
        table += 0x3C + (num_players + num_classes) * 4
    return d, modules

if __name__ == "__main__":
    d, mods = parse(sys.argv[1])
    for m in mods:
        h = m["header"]
        print(f"module {m['index']} @0x{m['at']:x} players={len(m['players'])} classes={m['classes']} head={h[:0x24].hex(' ')}")
        for j, p in enumerate(m["players"]):
            s = " ".join(f"#{no}(bnk{idx},pri{pr},az{az})" for no, idx, pr, az in p["sounds"])
            print(f"   player {j} @0x{p['offset']:x}{' (same table)' if p.get('dupe') else ''}: {s}")
