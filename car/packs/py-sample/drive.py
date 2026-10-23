# RumiCar の走行プログラム（見本）
#
# このファイルを書き換えると、自分の考えた走り方で RumiCar を走らせられます。
# 走行管理アプリの「自分のプログラムを送る…」で、このファイルのあるフォルダを
# 選ぶと車に送られ、そのまま有効になります（README「自分の走行プログラムを書く」）。
#
#   setup(car)  プログラムが始まるときに 1 回だけ呼ばれます（無くてもよい）
#   loop(car)   1 秒に 20 回（50 ミリ秒ごと）呼ばれます。ここで走り方を決めます
#
# car で使えるもの
#   car.left, car.center, car.right   左・前・右の距離センサーの値（mm）
#                                     何も近くに無いときは car.FAR（8190）、読めないときは None
#   car.params                        params.json に書いた値（例 car.params["speed"]）
#   car.time                          始まってからの秒数　　car.dt  前回の loop からの秒数
#   car.steer(car.LEFT / car.CENTER / car.RIGHT)   操舵（左・まっすぐ・右）
#   car.drive(数)                      駆動。-1000（全力で後退）〜 0（止まる）〜 1000（全力で前進）
#   car.brake()                        ブレーキ　　car.stop()  駆動を 0 に（惰性で止まる）
#   car.state("名前")                  今していることの名前（走行管理アプリの「判断状態」に出ます）
#   print(...)                         車のログに出ます（ssh で sudo journalctl -u rumicar-core -f）
#
# steer() と drive() で決めた値は、次に変えるまでそのまま続きます（Arduino と同じ）。
# 安全のため、本体が次のことを必ず行います。
#   ・「自動」にしたときは 0 から少しずつ速くする（いきなり全力では走り出さない）
#   ・走行管理アプリの「自動の最大出力」より速くは走らない
#   ・loop() が 30 ミリ秒以内に終わらないコマはブレーキ、1 秒答えが無ければプログラムを止める
#   ・エラーが出たコマはブレーキ（エラーの内容はログに出ます）


def setup(car):
    car.state("START")
    print("見本のプログラムを始めます。設定:", car.params)


def loop(car):
    p = car.params                       # params.json の値（車ごとに変えられます）
    left, center, right = car.left, car.center, car.right

    # センサーが読めないときは止まる
    if left is None or center is None or right is None:
        car.brake()
        car.state("NO_SENSOR")
        return

    if center < p["stop_mm"]:
        # 前がとても近い：開いている側の反対へハンドルを切って下がる（頭が開いている側を向く）
        if left > right:
            car.steer(car.RIGHT)
        else:
            car.steer(car.LEFT)
        car.drive(-p["back_speed"])
        car.state("BACK")
    elif center < p["turn_mm"]:
        # 前が近い：開いている側へ曲がりながら、ゆっくり進む
        if left > right:
            car.steer(car.LEFT)
        else:
            car.steer(car.RIGHT)
        car.drive(p["slow_speed"])
        car.state("TURN")
    else:
        # 前が開いている：横の壁が近ければ離れる、そうでなければまっすぐ
        if left < p["side_mm"]:
            car.steer(car.RIGHT)
        elif right < p["side_mm"]:
            car.steer(car.LEFT)
        else:
            car.steer(car.CENTER)
        car.drive(p["speed"])
        car.state("RUN")
