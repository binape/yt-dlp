#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace SmartQuant {

constexpr int CONFIRM_KLINE_COUNT = 2;
constexpr int DIVERGENCE_PERIOD = 5;
constexpr int MAX_DAILY_TRADE = 5;
constexpr int MAX_CONSECUTIVE_LOSS = 3;
constexpr int TRADE_COOLDOWN_SEC = 300;
constexpr double VOLATILITY_BREAK = 0.008;
constexpr double RSI_EXTREME_OVERSOLD = 20.0;
constexpr int FRIDAY_CUTOFF_HOUR = 14;
constexpr int FRIDAY_CUTOFF_MIN = 30;
constexpr int RECENT_HIGH_LOW_PERIOD = 20;
constexpr int PREMIUM_HISTORY_PERIOD = 10;
constexpr double PREMIUM_STD_TIMES = 1.5;
constexpr double DIVERGENCE_PRICE_BUFFER = 1.005;
constexpr double ATR_STOP_MULTIPLIER = 1.8;
constexpr double MIN_TRADE_VOLUME = 50000000.0;
constexpr int TIME_SYNC_THRESHOLD = 2;  // seconds
constexpr std::size_t MAX_CACHE_LEN = 60;
constexpr double MIN_VALUE_EPSILON = 1e-9;
constexpr double REGRESSION_SINGULAR_THRESHOLD = 1e-12;

enum class MarketState {
    RANGING,
    TRENDING_UP,
    TRENDING_DOWN,
};

enum class TradeSignal {
    SIGNAL_HOLD = 0,
    SIGNAL_BUY_T = 1,
    SIGNAL_SELL_T = -1,
    SIGNAL_SELL_ALL = -2,
};

struct LinearRegParam {
    double slope = 1.0;
    double fx_slope = 0.0;
    double intercept = 0.0;
};

struct DailyStat {
    int trade_count = 0;
    int consecutive_loss = 0;
    double daily_pnl = 0.0;
    double total_fee = 0.0;
    std::int64_t last_buy_time = 0;
    std::int64_t last_sell_time = 0;
    bool is_suspended = false;
};

struct TradeParam {
    double TotalCapital = 1000000.0;
    double MaxPositionRatio = 0.8;
    double BaseShare = 100.0;
    double T_Share = 100.0;
    double RSI_Buy_Threshold = 30.0;
    double RSI_Sell_Threshold = 70.0;
    double ETF_AnchorPrice = 1.0;
    double DailyMaxLoss = 10000.0;
    double ATR_StopMultiplier = ATR_STOP_MULTIPLIER;
};

struct MarketData {
    std::int64_t timestamp = 0;
    double london_gold_price = 0.0;
    double etf_price = 0.0;
    double fx_rate = 0.0;
    double premium = 0.0;
    double volume = 0.0;

    double adx_60m = 0.0;
    double ma20_slope_60m = 0.0;
    double rsi = 50.0;
    double atr = 0.0;
    double volatility = 0.0;

    bool is_kline_closed = false;

    std::deque<double> london_gold_close_history;
    std::deque<double> fx_rate_history;
    std::deque<double> etf_close_history;
    std::deque<double> premium_history;
    std::deque<double> rsi_history;
    std::deque<double> atr_history;
};

struct EngineStatus {
    MarketState market_state = MarketState::RANGING;
    TradeSignal signal = TradeSignal::SIGNAL_HOLD;
    double position_share = 0.0;
    double avg_cost = 0.0;
    DailyStat daily_stat;
    LinearRegParam reg_param;
    TradeParam trade_param;
};

using TradeCallback = std::function<void(double)>;
using SellAllCallback = std::function<void()>;
using AlertCallback = std::function<void(const std::string&)>;

void SmartQuant_Initialize(const TradeParam& param = {});
void SmartQuant_Destroy();
void SmartQuant_UpdateMarketData(const MarketData& data);
TradeSignal SmartQuant_GetSignal();
void SmartQuant_RegisterBuyCallback(TradeCallback cb);
void SmartQuant_RegisterSellCallback(TradeCallback cb);
void SmartQuant_RegisterSellAllCallback(SellAllCallback cb);
void SmartQuant_RegisterAlertCallback(AlertCallback cb);
void SmartQuant_DailyCloseRiskCheck();
EngineStatus SmartQuant_GetStatus();

}  // namespace SmartQuant
