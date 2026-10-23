% 合成脉冲脚本包入口：修改 pulse_config.m 后运行本脚本，无需 GUI/VBS。
package_dir = fileparts(mfilename('fullpath'));
addpath(package_dir);
cfg = pulse_config();
result = synthesize_pulses(cfg);
figures = plot_pulses(cfg, result);
output_dir = fullfile(package_dir, 'output');
if ~exist(output_dir, 'dir'), mkdir(output_dir); end
save(fullfile(output_dir, 'waveform.mat'), 'cfg', 'result', '-v7.3');
writetable(result.events, fullfile(output_dir, 'event_order.csv'));
for k = 1:numel(figures)
    exportgraphics(figures(k), fullfile(output_dir, sprintf('figure_%d.png',k)), 'Resolution',300);
    savefig(figures(k), fullfile(output_dir, sprintf('figure_%d.fig',k)));
end
if cfg.export_csv
    export_awg(fullfile(output_dir,'waveform.csv'),cfg,result,'noisy');
    if cfg.export_clean, export_awg(fullfile(output_dir,'waveform_clean.csv'),cfg,result,'clean'); end
end
fprintf('完成：%d 个脉冲（gamma %d / neutron %d），缩放除数 %.6g\n输出：%s\n', ...
    height(result.events),sum(result.kinds==0),sum(result.kinds==1),result.divisor,output_dir);
