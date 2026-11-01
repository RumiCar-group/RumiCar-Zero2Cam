# RumiCar の走行プログラム（見本）
#
# このファイルを書き換えることで、独自の走行方法で RumiCar を走行させることができます。
# 走行管理アプリの「自分のプログラムを送る…」でこのファイルを含むフォルダを選択すると、
# 車に送信され、そのまま有効化されます（README 11 章「自作の走行プログラム（Python）」）。
#
#   setup(car)  プログラムの開始時に 1 回だけ呼び出されます（省略可）
#   loop(car)   50 ミリ秒ごと（20 Hz）に呼び出されます。ここで操舵と駆動を決定します
#
# car の属性とメソッド
#   car.left, car.center, car.right   左・前・右の距離センサーの値（mm）
#                                     対象物を検出できなかった場合は car.FAR（8190）、読み取りに失敗した場合は None
#   car.params                        params.json に記述した値（例 car.params["speed"]）
#   car.time                          開始からの経過秒数　　car.dt  前回の loop からの経過秒数
#   car.steer(car.LEFT / car.CENTER / car.RIGHT)   操舵（左・中央・右）
#   car.drive(値)                      駆動。-1000（最大出力で後退）〜 0（停止）〜 1000（最大出力で前進）
#   car.brake()                        ブレーキ　　car.stop()  駆動を 0 にする（惰行で停止）
#   car.state("名前")                  現在の処理状態の名前（走行管理アプリの「判断状態」に表示されます）
#   print(...)                         車のログに記録されます（ssh で sudo journalctl -u rumicar-core -f）
#
# steer() と drive() で設定した値は、次に変更するまで保持されます（Arduino と同じ）。
# 安全のため、本体が次の制御を必ず行います。
#   ・自動モードへの切り替え時は、駆動の許容値を 0 から徐々に増加させる（急発進しない）
#   ・走行管理アプリの「自動の最大出力」を超えて走行しない
#   ・loop() の処理時間の上限は 30 ミリ秒。この時間内に処理が完了しなかったフレームはブレーキし、
#     1 秒以上応答が無ければプログラムを停止する
#   ・例外が発生したフレームはブレーキする（例外の内容はログに記録される）


def setup(car):
    car.state("START")
    print("見本のプログラムを始めます。設定:", car.params)


def loop(car):
    p = car.params                       # params.json の値（車ごとに変更可能）
    left, center, right = car.left, car.center, car.right

    # センサーの読み取りに失敗した場合は停止する
    if left is None or center is None or right is None:
        car.brake()
        car.state("NO_SENSOR")
        return

    if center < p["stop_mm"]:
        # 前方が非常に近い：距離の大きい側と反対へ操舵して後退する（車体の前部が距離の大きい側を向く）
        if left > right:
            car.steer(car.RIGHT)
        else:
            car.steer(car.LEFT)
        car.drive(-p["back_speed"])
        car.state("BACK")
    elif center < p["turn_mm"]:
        # 前方が近い：距離の大きい側へ操舵し、低速で前進する
        if left > right:
            car.steer(car.LEFT)
        else:
            car.steer(car.RIGHT)
        car.drive(p["slow_speed"])
        car.state("TURN")
    else:
        # 前方に余裕がある：側方の壁が近ければ反対側へ操舵し、そうでなければ直進する
        if left < p["side_mm"]:
            car.steer(car.RIGHT)
        elif right < p["side_mm"]:
            car.steer(car.LEFT)
        else:
            car.steer(car.CENTER)
        car.drive(p["speed"])
        car.state("RUN")
