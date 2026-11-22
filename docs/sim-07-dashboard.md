# SIM-07 — Dashboard web

## Objetivo

Mostrar as dez máquinas ao vivo, com alertas, e o histórico de cada uma, a
partir da API do backend em C (SIM-06). O dashboard é só HTML, CSS e
JavaScript, sem frameworks e sem CDN: funciona sem internet e não traz
dependências novas.

```text
Navegador ──GET /, /app.js, /style.css──▶ Backend em C (--web-root dashboard/)
          ──GET /api/machines/latest (5 s)─▶      │
          ──GET /api/machines/{id}/history─▶      └──▶ InterSCity (somente leitura)
```

## Decisões

| Tema | Decisão |
| --- | --- |
| Quem serve as páginas | O próprio backend em C, com a opção `--web-root PASTA`. Sem a opção, o backend se comporta como antes (todos os testes da SIM-06 seguem iguais). |
| Escopo da v1 | Visão ao vivo + histórico por máquina. |
| Alertas | Estado + falha prevista + limites, com as regras abaixo. |
| Atualização | `GET /api/machines/latest` a cada 5 s. O histórico aberto é consultado de novo só quando chega um dado mais recente da máquina. |
| Janelas do histórico | 1 h, 6 h e 24 h, contadas a partir do dado mais recente da máquina (os horários são do dataset, 2024, não do relógio). Janelas maiores passariam do limite de 1 MiB da resposta do backend (413). |

### Regras de alerta

O cartão de cada máquina mostra todos os alertas ativos; a cor da borda segue o
mais grave. Cor nunca aparece sozinha: cada alerta tem ícone e texto.

| Alerta | Regra | Gravidade |
| --- | --- | --- |
| Falha prevista (24 h) | `failure_within_24h = 1` | crítica |
| Temperatura alta | temperatura > 75 °C (5,5% das amostras do dataset) | séria |
| Vibração alta | `vibration_rms` > 3,8 (5,0% das amostras; unidade não informada no dataset) | séria |
| Sobrecarga | estado `PEAK` | aviso |
| Sem dados | nenhuma leitura da máquina no InterSCity | neutra |
| Normal | nenhuma das anteriores | normal |

Os limites ficam no início de `dashboard/app.js` (`LIMITS`).

## O que foi implementado

### Backend em C

- `backend/include/static_files.h` e `backend/src/static_files.c`: lê arquivos
  da pasta do dashboard com segurança.
  - `/` vira `/index.html`.
  - Aceita só letras, números, `.`, `_`, `-` e `/` na URL.
  - Recusa `..`, `.`, `//`, arquivos ocultos e barra no final.
  - Só entrega `.html`, `.css`, `.js`, `.svg`, `.png` e `.ico`, com o
    `Content-Type` certo.
  - Usa `realpath` para garantir que o arquivo está dentro da pasta, o que
    também bloqueia links simbólicos para fora.
  - Só arquivos comuns, até 2 MiB.
- `http_server.c`: com pasta configurada, GET e HEAD fora de `/api/` vão para o
  módulo; outros métodos recebem 405. Respostas levam `Cache-Control: no-cache` e
  `X-Content-Type-Options: nosniff`. Erros continuam em JSON.
- `main.c`: opção `--web-root PASTA`. Uma pasta inválida impede a inicialização,
  com a mensagem `pasta do dashboard inválida`.

### Dashboard (`dashboard/`)

- `index.html`, `style.css` e `app.js`.
- Resumo no topo: máquinas, normais, em alerta, falha prevista e sem dados.
- Cartão por máquina: tipo, alertas, temperatura, corrente, vibração RMS,
  rotação, estado, falha em 24 h e horário do último dado (UTC). Valores acima do
  limite aparecem em vermelho com ▲.
- Conexão: "Atualizado às…" quando tudo vai bem. Quando o backend responde 502,
  aparece "InterSCity indisponível"; quando o backend não responde, "Sem conexão
  com o backend". Se já havia dados, eles continuam na tela, esmaecidos.
- Histórico (clique no cartão):
  - Um gráfico por medida, com linha tracejada no limite e marcadores vermelhos
    nas leituras acima dele.
  - Faixas de tempo para estado e falha prevista.
  - Linha vertical com dica ao passar o mouse.
  - Botão "Tabela" com todas as leituras.
- Tema escuro automático (segue o sistema) e layout para celular.
- Todo texto vindo da API entra na página por `textContent`.

### Ferramenta de teste: `dashboard/tests/fake_api.py`

API falsa no mesmo formato do backend, alimentada pelo CSV. O relógio do dataset
anda 3 minutos a cada 5 s reais, como se o replayer estivesse enviando. Serve
também o dashboard. Não acessa o InterSCity.

    python3 dashboard/tests/fake_api.py --port 8090 --no-data 13
    # abra http://127.0.0.1:8090/
    # /__fake/down simula InterSCity fora (502); /__fake/up volta ao normal

## Testes

| Teste | O que cobre | Onde rodou |
| --- | --- | --- |
| `backend/tests/test_static_files.c` (30 verificações) | Tipos, `/` → index, subpastas, arquivo vazio, `..`, `.`, `//`, ocultos, extensões proibidas, links para fora, URL longa, `%`, caracteres de controle e arquivo acima do limite | Ambiente de desenvolvimento do Claude, também com AddressSanitizer e UBSan, sem erros nem vazamentos |
| `backend/tests/test_dashboard_static.py` (9 verificações) | Backend real com `--web-root`: tipos e cabeçalhos, HEAD, 7 caminhos proibidos → 404 JSON (incluindo `%2e%2e`), POST → 405, `/api/` intacta, sem a opção nada é servido, pasta inválida impede a inicialização | **Pendente: rodar com `make -C backend test`** (a libmicrohttpd não estava disponível no ambiente onde o código foi escrito) |
| Navegador (Chrome sem interface) contra `fake_api.py` | Visão geral; histórico 1 h, 6 h e 24 h; dica do mouse; tabela; máquina sem dados; 502 sem dados anteriores; 502 com dados (cartões esmaecidos) e recuperação; histórico acompanhando dados novos; tema escuro; celular; console sem erros de JavaScript | Ambiente de desenvolvimento do Claude |

## Como rodar

Com Compose (o backend serve o dashboard; o InterSCity padrão é o endereço
inacessível `http://127.0.0.1:9`, então a tela mostra "InterSCity indisponível"):

    docker compose up -d --build backend
    # abra http://127.0.0.1:8080/

Sem Docker, a partir da raiz do repositório:

    make -C backend
    backend/build/backend --port 8080 --machines config/interscity/machines.conf \
        --interscity-url http://127.0.0.1:9 --web-root dashboard

Sem `--interscity-url`, o backend usa por padrão a instância da UFMA (somente
leitura). Apontar o dashboard para ela é uma decisão da equipe.

## Limitações conhecidas

- O teste de integração do backend com `--web-root` ainda precisa rodar na máquina
  da equipe.
- O dashboard ainda não foi visto com dados reais vindos do InterSCity. Até um envio
  real com o contrato da SIM-11, vibração e rotação aparecem como "—".
- Os tipos das máquinas (CNC, Bomba, Compressor) estão fixos em `app.js`, porque a
  API não os devolve.
- Sem autenticação: o backend continua escutando só em `127.0.0.1` por padrão
  (no Compose, a porta é publicada só em `127.0.0.1`).
