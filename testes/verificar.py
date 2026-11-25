"""Confere o exemplo do professor em builds isolados. Execute com make testar."""
import hashlib
import re
import shutil
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory

PROJETO = Path(__file__).resolve().parents[1]
GABARITO = "05574ab43f7834a48696eb2cc6cee770c37959e5fe464760580946952f874106"
LINHA = re.compile(r"Virtual address: (\d+) TLB: (-?\d+) Physical address: (\d+) Value: (-?\d+)")


def compilar(pasta, opcoes=""):
    subprocess.run(["make", "-B", "vm", "CPPFLAGS=" + opcoes], cwd=pasta, check=True)


def executar(pasta, memoria, tlb):
    subprocess.run(["./vm", "address.txt", memoria, tlb], cwd=pasta, check=True, timeout=90)
    return (pasta / "correct.txt").read_bytes()


def verificar_sem_tlb(saida, enderecos, disco, capacidade, politica):
    # O dicionário conserva a ordem de inserção. Para LRU, recolocamos a página acessada no fim.
    residentes = {}
    faltas = 0
    linhas = saida.decode("ascii").splitlines()
    for i, endereco in enumerate(enderecos):
        pagina, deslocamento = divmod(endereco, 256)
        if pagina not in residentes:
            faltas += 1
            if len(residentes) < capacidade:
                frame = len(residentes)
            else:
                frame = residentes.pop(next(iter(residentes)))
            residentes[pagina] = frame
        frame = residentes[pagina]
        if politica == "lru":
            del residentes[pagina]
            residentes[pagina] = frame
        byte = int.from_bytes(disco[endereco:endereco + 1], "big", signed=True)
        campos = LINHA.fullmatch(linhas[i])
        if not campos or tuple(map(int, campos.groups())) != (endereco, -1, frame * 256 + deslocamento, byte):
            raise ValueError(f"Divergência na tradução {i + 1}: {linhas[i]}")
    n = len(enderecos)
    esperado = [f"Number of Translated Addresses = {n}", f"Page Faults = {faltas}",
                f"Page Fault Rate = {faltas/n:.3f}", "TLB Hits = 0", "TLB Hit Rate = 0.000"]
    if linhas[n:] != esperado:
        raise ValueError(f"Estatísticas inesperadas: {linhas[n:]}")
    print(f"OK sem TLB: {capacidade} frames, {politica}, {faltas} faltas", flush=True)


def verificar(pasta):
    for nome in ["Makefile", "vm.c", "vm.h", "address.txt", "BACKING_STORE.bin"]:
        shutil.copyfile(PROJETO / nome, pasta / nome)
    enderecos = list(map(int, (pasta / "address.txt").read_text().split()))
    disco = (pasta / "BACKING_STORE.bin").read_bytes()

    for capacidade in (256, 128):
        compilar(pasta, f"-DFRAMES={capacidade} -DMODO_TLB=SEM_TLB")
        for politica in ("fifo", "lru"):
            verificar_sem_tlb(executar(pasta, politica, "fifo"), enderecos, disco, capacidade, politica)

    compilar(pasta, "-DMODO_TLB=TLB_SEQUENCIAL")
    sequencial = {(m, t): executar(pasta, m, t) for m in ("fifo", "lru") for t in ("fifo", "lru")}
    compilar(pasta)
    for (m, t), esperado in sequencial.items():
        saida = executar(pasta, m, t)
        if saida != esperado:
            raise ValueError(f"Buscas sequencial e paralela divergem para {m}/{t}")
        if (m, t) == ("lru", "fifo") and hashlib.sha256(saida).hexdigest() != GABARITO:
            raise ValueError("Saída lru/fifo diverge do gabarito do professor.")
        print(f"OK TLB: {m}/{t}, saídas sequencial e paralela idênticas", flush=True)
    print("OK gabarito lru/fifo: SHA-256", GABARITO, flush=True)


if __name__ == "__main__":
    with TemporaryDirectory(prefix="teste-vm-") as tmp:
        verificar(Path(tmp))
