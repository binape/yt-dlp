#include "SmartQuant.h"

#include <cassert>
#include <ctime>

int main() {
    using namespace SmartQuant;

    TradeParam param;
    param.TotalCapital = 2000000.0;
    param.MaxPositionRatio = 0.9;
    param.T_Share = 200.0;

    SmartQuant_Initialize(param);

    double bought = 0.0;
    double sold = 0.0;
    bool sold_all = false;
    bool alerted = false;

    SmartQuant_RegisterBuyCallback([&](double qty) { bought += qty; });
    SmartQuant_RegisterSellCallback([&](double qty) { sold += qty; });
    SmartQuant_RegisterSellAllCallback([&]() { sold_all = true; });
    SmartQuant_RegisterAlertCallback([&](const std::string&) { alerted = true; });

    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    for (int i = 0; i < 12; ++i) {
        MarketData md;
        md.timestamp = now;
        md.london_gold_price = 560.0 - i;
        md.etf_price = 1.0 + 0.001 * i;
        md.fx_rate = 7.2;
        md.premium = -0.01;
        md.volume = 100000000.0;
        md.adx_60m = 30.0;
        md.ma20_slope_60m = 0.4;
        md.rsi = 18.0;
        md.atr = 0.01;
        md.volatility = 0.002;
        md.is_kline_closed = true;
        SmartQuant_UpdateMarketData(md);
    }

    const auto status = SmartQuant_GetStatus();
    assert(status.signal == TradeSignal::SIGNAL_BUY_T || status.signal == TradeSignal::SIGNAL_HOLD);
    assert(status.position_share >= 0.0);
    assert(bought >= 0.0);

    MarketData stale_md;
    stale_md.timestamp = now - (TIME_SYNC_THRESHOLD + 10);
    stale_md.london_gold_price = 550.0;
    stale_md.etf_price = 1.1;
    stale_md.fx_rate = 7.2;
    stale_md.premium = -0.01;
    stale_md.volume = 100000000.0;
    stale_md.adx_60m = 30.0;
    stale_md.ma20_slope_60m = 0.4;
    stale_md.rsi = 18.0;
    stale_md.atr = 0.01;
    stale_md.volatility = 0.002;
    stale_md.is_kline_closed = true;
    SmartQuant_UpdateMarketData(stale_md);
    assert(SmartQuant_GetSignal() == TradeSignal::SIGNAL_HOLD);

    MarketData break_md = stale_md;
    break_md.timestamp = now;
    break_md.volatility = VOLATILITY_BREAK;
    break_md.rsi = RSI_EXTREME_OVERSOLD;
    SmartQuant_UpdateMarketData(break_md);
    const auto broken = SmartQuant_GetStatus();
    assert(broken.daily_stat.is_suspended);

    SmartQuant_DailyCloseRiskCheck();
    const auto after = SmartQuant_GetStatus();
    assert(after.daily_stat.trade_count == 0);
    assert(after.daily_stat.daily_pnl == 0.0);

    SmartQuant_Destroy();
    assert(sold >= 0.0);
    return 0;
}
