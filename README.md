# TUPAC

Terminal tabanlı paket yöneticisi. Pacman, DNF ve APT destekler.

## Özellikler

- Arama çubuğuna yazarak paket ara
- Yön tuşları ile listede gezin
- Enter ile kur/kaldır (onay pencereli)
- Şifre güvenli popup ile girilir
- Arka plan işlemlerinde UI donmaz
- Arch, Fedora, Debian ve türevlerinde çalışır

## Kurulum

### Curl ile

bash
curl -sSL https://raw.githubusercontent.com/vrchester/tupac/main/install.sh | bash


### Elle derleme

bash
git clone https://github.com/vrchester/tupac.git
cd tupac
make
sudo make install


## Kullanım

bash
tupac


| Tuş        | İşlev                         |
|------------|-------------------------------|
| `↑` `↓`   | Paket listesinde gezin        |
| `j` `k`    | Vim usulü gezinme            |
| yazarak    | Paket ara                     |
| `Enter`    | Kur veya kaldır (onay ister) |
| `Backspace`| Aramadan harf sil            |
| `q`        | Çıkış                         |

## Gereksinimler

- g++ (C++17)
- libncursesw
- Sisteminizde kurulu bir paket yöneticisi (pacman, dnf veya apt)

## Lisans

GPL-3.0
