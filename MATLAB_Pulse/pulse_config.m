function c = pulse_config()
% 所有可编辑参数集中于此。时间单位 ns，幅度为相对单位。
c.mode = 'mixed'; % mixed / gamma / neutron / custom
c.fs = 75; % MSa/s
c.points = 3200000;
c.window = 2800;
c.gap = 200;
c.rise = 3;
c.peak = 0.8;
c.noise = 0.02; % 缩放前全记录高斯噪声 RMS
c.baseline = 0;
c.seed = 20261012;
c.amplitude_jitter = 5; % ±百分比，逐事件均匀分布
c.tau_jitter = 10; % 三个衰减常数及上升常数分别独立波动
c.taper = 2500;
c.gamma_taus = [13 35 270]; c.gamma_weights = [.80 .17 .03];
c.neutron_taus = [13 59 460]; c.neutron_weights = [.65 .27 .08];
c.custom_taus = [13 35 270]; c.custom_weights = [.80 .17 .03];
c.normalize = true;
c.preview_events = 12;
c.export_csv = true;
c.export_clean = false;
end
