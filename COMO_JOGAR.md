# Splatoon Raiders (recomp): como jogar

1. Abra o **`Launcher.exe`**. Em **Arquivos do jogo**, escolha a sua cópia do jogo (atualização 65536): a pasta com
   `exefs` e `romfs`, ou a pasta acima dela com `updates/65536`. O launcher confere a versão.
2. Se o `SplatoonRaiders.exe` ainda não existe, clique em **Configurar**: o launcher converte o código do jogo e o
   compila neste PC, uma vez só (de 30 a 70 minutos, uns 5 GB livres; se for interrompido, continua de onde parou).
   Depois ele traduz os shaders do jogo (cerca de 1 minuto).
3. Clique em **Jogar**. As opções ficam em **Configurações** (Tela, Imagem, Desempenho, Controles, Geral).

**Requisitos**: Windows 10 ou 11 de 64 bits, placa de vídeo com Vulkan 1.3 e driver atualizado, e a sua cópia do
jogo. O `SplatoonRaiders.exe` gerado contém código derivado do jogo: é para uso pessoal, com a sua cópia; não
compartilhe.

**Windows Defender**: o `Launcher.exe` não tem assinatura digital e às vezes é acusado por engano
(`Program:Win32/Contebrew.A!ml`). Para liberar: Segurança do Windows > Proteção contra vírus e ameaças > Histórico
de proteção > o item > Ações > Permitir no dispositivo. Não abra o launcher de dentro do WinRAR: extraia antes.

Se algo der errado, o registro da última sessão fica em `logs\splatoon.log`, ao lado do programa (o da configuração,
em `setup_work\setup.log`).

## Configurações (menu inicial)

| Opção | O que faz |
|---|---|
| Resolução da janela | Tamanho da janela (1280x720 até 3840x2160). O tamanho do seu monitor abre em tela cheia; maior que a tela, a janela se ajusta. A imagem mantém 16:9 (faixas pretas se a janela tiver outro formato) |
| Modo de exibição | Janela ou tela cheia sem bordas |
| Modo do console | **TV**: o jogo gera imagem 4K (mais bonito). **Portátil**: 1280x720 (mais leve) |
| Escala da imagem | **AMD FSR 1**, **NVIDIA Image Scaling (NIS)** (mais nítidos) ou linear (mais suave) |
| Nitidez | Quanto o FSR / NIS realçam os contornos: nenhuma, média, alta (recomendado) ou máxima |
| Limite de FPS | 30, 60 (recomendado), 120 (experimental) ou sem limite |
| VSync | Desligado tem menos atraso; ligado evita "tearing" |
| Overlay de desempenho | Desligado, só FPS, ou completo (FPS, tempos, CPU, GPU, memória) |
| Controle | Automático (Xbox, PlayStation, teclado) ou só teclado |
| Botões | Posição do Switch (A à direita) ou Xbox/PlayStation (A embaixo) |
| Vibração | Desligada, fraca ou forte |
| Mira por movimento | Como no Switch: mova o DualSense / DualShock 4 para mirar (giroscópio). Pode desligar |
| Mira vertical (analógico R) | O jogo mira para cima e para baixo com o movimento do controle; aqui o analógico direito faz esse papel. Baixa, média ou alta; desligue se tirar os controles de movimento nas opções do jogo |
| Pasta do jogo | A pasta do jogo com as subpastas `exefs` e `romfs` (atualização 65536). Clique para escolher |
| Idioma | Português ou inglês (automático segue o idioma do Windows); vale para o menu e o overlay |
| Mostrar este menu ao iniciar | "Não" abre o jogo direto. Segure **Shift** ao abrir para ver o menu de novo |

As escolhas ficam em `splatoon.ini`, ao lado do programa (dá para editar à mão). Os saves ficam na pasta
`profile`, também ao lado do programa.

## Desempenho e resolução

- No modo **TV** o jogo sempre desenha em 2560x1440 e amplia para 4K; o tamanho da janela quase não muda o
  custo. Em 1440p (testado numa RTX 5060) roda a 60 fps estáveis, igual a 720p.
- **Primeira vez em cada área**: efeitos novos precisam ter os shaders traduzidos, o que pode dar uma travadinha
  rápida. As traduções ficam guardadas em `shader_cache.bin`, ao lado do programa, e da segunda vez em diante
  essas travadinhas somem. O arquivo pode ser apagado com segurança (o jogo cria outro).

- **Preparação de shaders**: ao escolher os arquivos do jogo, o `Launcher.exe` traduz uma vez todos os shaders do jogo
  (cerca de 24 mil, 1 minuto; ficam em `shader_cache.bin`, uns 2 GB), para o jogo não travar ao mostrar algo pela
  primeira vez. Antes de cada partida ele também compila os shaders já vistos (`pipeline_cache.bin`). O botão
  **Preparar shaders** refaz a tradução.

## Teclas durante o jogo

| Tecla | Função |
|---|---|
| F1 | Liga/desliga o overlay (desligado → FPS → completo) |
| F11 ou Alt+Enter | Tela cheia / janela |

## Controles

- **Xbox** (ou qualquer controle XInput, inclusive via Steam): funciona direto.
- **PlayStation** (DualShock 4, DualSense, DualSense Edge): USB ou Bluetooth, com vibração nos dois.
- Os controles podem ser ligados e desligados com o jogo aberto.

Teclado:

| Tecla | Botão do Switch |
|---|---|
| W A S D | Analógico esquerdo (andar) |
| I J K L | Analógico direito (câmera) |
| Enter / Espaço | A |
| Backspace / Esc | B |
| C | X |
| V | Y (volta a câmera para trás do personagem) |
| Q / E | L / R |
| 1 / 3 | ZL (lula) / ZR (atirar) |
| P / M | + / − |
| F / H | Clique dos analógicos |
| Setas | Direcional |
