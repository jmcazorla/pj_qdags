#!/usr/bin/env bash
# Importa las clases de ccqQDAGS-high_arity_qdag (rank_bv_64, se_quadtree, qdag, initializable_array) a
# include/qdags/ccq/ como `namespace ccq`; reproducible si cambia su repositorio. Transformaciones (sin macros):
#   1. abre `namespace ccq {` despues del ultimo #include y lo cierra antes del ultimo #endif; las
#      `using namespace` quedan DENTRO del namespace (no contaminan el global);
#   2. en se_quadtree y qdag, `private:` -> `protected:` (los adaptadores de ccq/adapters.hpp heredan);
#   3. N2: rank_bv_64::select_next y select_next_active devuelven `u` si `i >= u` (la version original lee
#      seq[i/64] fuera del bitvector si el ultimo nodo de un nivel termina en una frontera de 64 bits y su ultimo bit
#      es 1; pasa con cualquier arbol);
#   4. memoria: rank_bv_64::release() (la clase no tiene destructor) y ~qdag con delete[] M y liberacion de
#      M_prime (el original hace `delete M` sobre un arreglo, UB, y no libera M_prime). Sus drivers nunca borran un
#      qdag; las estrategias de pj_qdags si.
# No se aplica N1 (que get_children_lastlevel respete `active`): solo importa con un indice reducido por semijoin.
# N3 (join vacio) ya esta corregido en su version vigente.
#
# Uso:  scripts/import_ccq.sh <dir_ccqQDAGS-high_arity_qdag>
set -euo pipefail
SRC="${1:?Usage: $0 <ccqQDAGS-high_arity_qdag_dir>}/includes"
DST="$(cd "$(dirname "$0")/.." && pwd)/include/qdags/ccq"
mkdir -p "$DST"
for f in initArray rank se_quadtree qdags; do
  [ -f "$SRC/$f.hpp" ] || { echo "missing $SRC/$f.hpp" >&2; exit 1; }
  python3 -I - "$SRC/$f.hpp" "$DST/$f.hpp" "$f" <<'PY'
import re, sys
src, dst, name = sys.argv[1:4]
lines = open(src, encoding="utf-8").read().split("\n")
last_inc = max(i for i, l in enumerate(lines) if re.match(r"\s*#\s*include", l))
last_endif = max(i for i, l in enumerate(lines) if re.match(r"\s*#\s*endif", l))
out = lines[:last_inc + 1] + ["", "namespace ccq {", ""] + lines[last_inc + 1:last_endif] + ["", "} // namespace ccq", ""] + lines[last_endif:]
text = "\n".join(out)
if name in ("se_quadtree", "qdags"):
    text = re.sub(r"(\n\s*)private:", r"\1protected:", text)
if name == "rank":
    for sig in ("uint64_t select_next(uint64_t i)\n    {\n", "uint64_t select_next_active(uint64_t i, rank_bv_64 active)\n    {\n"):
        assert text.count(sig) == 1, "no se pudo aplicar N2"
        text = text.replace(sig, sig + "        if (i >= u) return u;   // N2 (parche de pj_qdags; ver scripts/import_ccq.sh)\n")
    old = "    void serialize(std::ostream &out) const {"
    assert text.count(old) == 1
    text = text.replace(old, "    // parche de pj_qdags: libera los arreglos (la clase no tiene destructor)\n    void release() { delete[] seq; delete[] block; seq = nullptr; block = nullptr; }\n\n" + old)
if name == "qdags":
    old = "if (is_extended_qdag) delete M;"
    assert old in text
    text = text.replace(old, "// parche de pj_qdags: el original hacia `delete M` sobre un arreglo (UB) y no liberaba M_prime\n        delete[] M;\n        for (auto *v : M_prime) delete v;")
hdr = "// IMPORTADO de ccqQDAGS-high_arity_qdag/includes/%s.hpp por scripts/import_ccq.sh -- NO EDITAR A MANO.\n" % name
open(dst, "w", encoding="utf-8").write(hdr + text)
PY
done
echo "imported into $DST"
