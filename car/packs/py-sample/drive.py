# RumiCar の走行プログラム（見本）
#
# このファイルを書き換えることで、独自の走行方法で RumiCar を走行させることができます。
# 走行管理アプリの「自分のプログラムを送る…」でこのファイルを含むフォルダを選択すると、
# 車に送信され、そのまま有効化されます（README 11 章「自作の走行プログラム（Python）」）。
#
# 使うのは RumiCar ライブラリです。Raspberry Pi 版のライブラリ（RumiCar リポジトリの
# RasPi/RumiCar.py 0.7.0）そのものを車の本体が使っており、Arduino のライブラリ
# （RumiCar-lib v0.7.0）とも同じ名前・同じ値です。
#
#   setup()   プログラムの開始時に 1 回だけ呼び出されます（省略可）
#   loop()    50 ミリ秒ごと（20 Hz）に呼び出されます。ここで操舵と駆動を決定します
#
#   RC_setReadLimit(上限)            RC_read の値を信頼する距離の上限（mm）を決めます。setup() で 1 回呼びます。
#                                    上限は 1〜2000、0 は上限なし（既定。2000 mm まで返る）。
#                                    RC_setReadLimit(方向, 上限) と書くと、その方向（LEFT / CENTER / RIGHT）だけに設定します
#   RC_read(LEFT / CENTER / RIGHT)   左・前・右の距離センサーの値（mm）
#                                    0〜上限 = 距離、-1 = 読み取りに失敗、
#                                    -3 = 範囲外（RC_setReadLimit で決めた上限より先、または対象物なし）。走行では「遠い」として扱います
#                                    -3 は数としては最も小さいので、比較の前に、RC_read が返すどの距離よりも大きい値に置き換えます
#   RC_steer(LEFT / CENTER / RIGHT, 強さ)
#                                    操舵（左・中央・右）。強さは 0〜255 の信号の割合で、省略すると 255（最大）。
#                                    0 は CENTER と同じ（中立）、CENTER では強さは無視される。
#                                    補正はしないため、同じ値でも車ごとに舵の効き方は異なる
#   RC_drive(方向, 値)                駆動。方向は FREE（惰行）/ REVERSE（後退）/ FORWARD（前進）/ BRAKE（ブレーキ）、
#                                    値は 0〜255 の PWM のデューティ（150 なら全開の 150/255 ≒ 59 % の信号）
#   RC_params()                      params.json に記述した値（例 RC_params()["speed"]）
#   RC_state("名前")                 現在の処理状態の名前（走行管理アプリの「判断状態」に表示されます）
#   print(...)                       車のログに記録されます（ssh で sudo journalctl -u rumicar-core -f）
#
# 距離の上限について：センサーのレーザーはコーン状に広がるため、取り付け位置や車高によっては、
# ある距離から先で床を捉えます。どこまでの値を信頼するかは、ライブラリではなくプログラムが
# RC_setReadLimit で決めます。この見本は params.json の read_limit_mm を上限にしています
# （値の決め方は README 5 章「ToF の値を信頼する距離の上限」）。距離のしきい値（stop_mm・turn_mm・
# side_mm）は上限以下にします。上限より先は -3 になり、しきい値と比べても「遠い」としか分からないためです。
#
# RC_steer() と RC_drive() で設定した値は、次に変更するまで保持されます（Arduino と同じ）。
# この見本の操舵は強さを省略した最大（255）です。強さを変えて走りを滑らかにする例は、
# RumiCar リポジトリの RasPi/Exercise-3.3（なめらかに走る車）にあります。
# 安全のため、本体が次の制御を必ず行います。
#   ・自動モードへの切り替え時は、駆動の許容値を 0 から徐々に増加させる（急発進しない）
#   ・走行管理アプリの「自動の最大出力」を超えて走行しない
#   ・loop() の処理時間の上限は 30 ミリ秒。この時間内に処理が完了しなかったフレームはブレーキし、
#     1 秒以上応答が無ければプログラムを停止する
#   ・例外が発生したフレームはブレーキする（例外の内容はログに記録される）

from RumiCar import *

# -3（範囲外）を比較で「遠い」として扱うための値。RC_read が返すどの距離（最大 2000 mm）よりも
# 大きいので、read_limit_mm をいくつにしても上限より大きい
FAR = 9999


def setup():
    RC_setup()
    # RC_read の値を信頼する距離の上限（mm）。これより先は -3（遠い）になる。値は params.json の read_limit_mm
    RC_setReadLimit(RC_params().get("read_limit_mm", 200))
    RC_state("START")
    print("見本のプログラムを始めます。設定:", RC_params())


def loop():
    p = RC_params()                      # params.json の値（車ごとに変更可能）
    left = RC_read(LEFT)
    center = RC_read(CENTER)
    right = RC_read(RIGHT)

    # センサーの読み取りに失敗した場合は停止する
    if left == -1 or center == -1 or right == -1:
        RC_drive(BRAKE, 0)
        RC_state("NO_SENSOR")
        return

    # 範囲外（-3：上限より先、または対象物なし）は「遠い」
    if left == -3:
        left = FAR
    if center == -3:
        center = FAR
    if right == -3:
        right = FAR

    if center < p["stop_mm"]:
        # 前方が非常に近い：距離の大きい側と反対へ操舵して後退する（車体の前部が距離の大きい側を向く）
        if left > right:
            RC_steer(RIGHT)
        else:
            RC_steer(LEFT)
        RC_drive(REVERSE, p["back_speed"])
        RC_state("BACK")
    elif center < p["turn_mm"]:
        # 前方が近い：距離の大きい側へ操舵し、低速で前進する
        if left > right:
            RC_steer(LEFT)
        else:
            RC_steer(RIGHT)
        RC_drive(FORWARD, p["slow_speed"])
        RC_state("TURN")
    else:
        # 前方に余裕がある：左右の壁が近ければ反対側へ操舵し、そうでなければ直進する
        if left < p["side_mm"]:
            RC_steer(RIGHT)
        elif right < p["side_mm"]:
            RC_steer(LEFT)
        else:
            RC_steer(CENTER)
        RC_drive(FORWARD, p["speed"])
        RC_state("RUN")
