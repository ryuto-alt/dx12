-- time API の検証用(検証後に削除してOK)
properties = {
  { name = "tNow",   type = "float", default = -1, label = "time.now" },
  { name = "tReal",  type = "float", default = -1, label = "time.realtime" },
  { name = "tFrame", type = "float", default = -1, label = "time.frame" },
  { name = "fired",  type = "float", default = 0,  label = "after発火回数" },
  { name = "ticks",  type = "float", default = 0,  label = "every発火回数" },
  { name = "phase",  type = "float", default = 0 },
}

function OnStart(self)
  time.after(0.3, function() self.fired = self.fired + 1 end)
  self.everyId = time.every(0.2, function() self.ticks = self.ticks + 1 end)
end

function OnUpdate(self, dt)
  self.tNow   = time.now()
  self.tReal  = time.realtime()
  self.tFrame = time.frame()
  -- 1秒経ったらポーズ(setScale(0))。以降 tNow は止まり tReal だけ進めば合格
  if self.phase == 0 and time.now() > 1.0 then
    self.phase = 1
    time.cancel(self.everyId)
    time.setScale(0)
  end
end