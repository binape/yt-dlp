#include "SmartQuant.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <limits>
#include <mutex>
#include <numeric>
#include <vector>

namespace SmartQuant {
namespace {

template <typename T>
void PushCache(std::deque<T>& q, T value) {
    q.push_back(value);
    while (q.size() > MAX_CACHE_LEN) {
        q.pop_front();
    }
}

template <typename T>
bool IsFinitePositive(T v) {
    return std::isfinite(static_cast<double>(v)) && v > 0;
}

class Engine {
public:
    void Initialize(const TradeParam& param) {
        std::lock_guard<std::mutex> lock(mu_);
        param_ = param;
        daily_stat_ = {};
        data_ = {};
        market_state_ = MarketState::RANGING;
        current_signal_ = TradeSignal::SIGNAL_HOLD;
        reg_param_ = {};
        position_share_ = 0.0;
        avg_cost_ = 0.0;
        last_trade_ts_ = 0;
    }

    void Destroy() {
        std::lock_guard<std::mutex> lock(mu_);
        daily_stat_ = {};
        data_ = {};
        market_state_ = MarketState::RANGING;
        current_signal_ = TradeSignal::SIGNAL_HOLD;
        reg_param_ = {};
        position_share_ = 0.0;
        avg_cost_ = 0.0;
        last_trade_ts_ = 0;
        buy_cb_ = nullptr;
        sell_cb_ = nullptr;
        sell_all_cb_ = nullptr;
        alert_cb_ = nullptr;
    }

    void RegisterBuy(TradeCallback cb) {
        std::lock_guard<std::mutex> lock(mu_);
        buy_cb_ = std::move(cb);
    }

    void RegisterSell(TradeCallback cb) {
        std::lock_guard<std::mutex> lock(mu_);
        sell_cb_ = std::move(cb);
    }

    void RegisterSellAll(SellAllCallback cb) {
        std::lock_guard<std::mutex> lock(mu_);
        sell_all_cb_ = std::move(cb);
    }

    void RegisterAlert(AlertCallback cb) {
        std::lock_guard<std::mutex> lock(mu_);
        alert_cb_ = std::move(cb);
    }

    void UpdateMarketData(const MarketData& new_data) {
        std::lock_guard<std::mutex> lock(mu_);
        data_.timestamp = new_data.timestamp;
        data_.london_gold_price = new_data.london_gold_price;
        data_.etf_price = new_data.etf_price;
        data_.fx_rate = new_data.fx_rate;
        data_.premium = new_data.premium;
        data_.volume = new_data.volume;
        data_.adx_60m = new_data.adx_60m;
        data_.ma20_slope_60m = new_data.ma20_slope_60m;
        data_.rsi = new_data.rsi;
        data_.atr = new_data.atr;
        data_.volatility = new_data.volatility;
        data_.is_kline_closed = new_data.is_kline_closed;

        if (new_data.is_kline_closed) {
            PushCache(data_.london_gold_close_history, new_data.london_gold_price);
            PushCache(data_.fx_rate_history, new_data.fx_rate);
            PushCache(data_.etf_close_history, new_data.etf_price);
            PushCache(data_.premium_history, new_data.premium);
            PushCache(data_.rsi_history, new_data.rsi);
            PushCache(data_.atr_history, new_data.atr);
        }

        current_signal_ = GenerateTradeSignal();
    }

    TradeSignal GetSignal() {
        std::lock_guard<std::mutex> lock(mu_);
        return current_signal_;
    }

    void DailyCloseRiskCheck() {
        std::lock_guard<std::mutex> lock(mu_);
        if (daily_stat_.daily_pnl <= -std::abs(param_.DailyMaxLoss)) {
            daily_stat_.is_suspended = true;
            SendAlert("Daily max loss exceeded, strategy suspended");
        }
        daily_stat_.trade_count = 0;
        daily_stat_.consecutive_loss = 0;
        daily_stat_.daily_pnl = 0.0;
        daily_stat_.total_fee = 0.0;
        if (daily_stat_.is_suspended && data_.volatility < VOLATILITY_BREAK) {
            daily_stat_.is_suspended = false;
        }
        UpdateMappingByRegression();
        UpdateRSIThreshold(GetMarketState());
    }

    EngineStatus GetStatus() {
        std::lock_guard<std::mutex> lock(mu_);
        EngineStatus status;
        status.market_state = market_state_;
        status.signal = current_signal_;
        status.position_share = position_share_;
        status.avg_cost = avg_cost_;
        status.daily_stat = daily_stat_;
        status.reg_param = reg_param_;
        status.trade_param = param_;
        return status;
    }

private:
    MarketState GetMarketState() {
        if (data_.adx_60m >= 25.0 && data_.ma20_slope_60m > 0) {
            return MarketState::TRENDING_UP;
        }
        if (data_.adx_60m >= 25.0 && data_.ma20_slope_60m < 0) {
            return MarketState::TRENDING_DOWN;
        }
        return MarketState::RANGING;
    }

    void UpdateRSIThreshold(MarketState state) {
        switch (state) {
        case MarketState::TRENDING_UP:
            param_.RSI_Buy_Threshold = 35.0;
            param_.RSI_Sell_Threshold = 75.0;
            break;
        case MarketState::TRENDING_DOWN:
            param_.RSI_Buy_Threshold = 25.0;
            param_.RSI_Sell_Threshold = 65.0;
            break;
        default:
            param_.RSI_Buy_Threshold = 30.0;
            param_.RSI_Sell_Threshold = 70.0;
            break;
        }
    }

    double GetRecentLow() const {
        if (data_.london_gold_close_history.empty()) {
            return data_.london_gold_price;
        }
        const auto n = std::min<std::size_t>(RECENT_HIGH_LOW_PERIOD, data_.london_gold_close_history.size());
        return *std::min_element(data_.london_gold_close_history.end() - static_cast<std::ptrdiff_t>(n),
                                 data_.london_gold_close_history.end());
    }

    double GetRecentHigh() const {
        if (data_.london_gold_close_history.empty()) {
            return data_.london_gold_price;
        }
        const auto n = std::min<std::size_t>(RECENT_HIGH_LOW_PERIOD, data_.london_gold_close_history.size());
        return *std::max_element(data_.london_gold_close_history.end() - static_cast<std::ptrdiff_t>(n),
                                 data_.london_gold_close_history.end());
    }

    void UpdateMappingByRegression() {
        const auto n = std::min({data_.etf_close_history.size(), data_.london_gold_close_history.size(), data_.fx_rate_history.size()});
        if (n < 3) {
            return;
        }

        double s_x1 = 0.0, s_x2 = 0.0, s_y = 0.0;
        double s_x1x1 = 0.0, s_x2x2 = 0.0, s_x1x2 = 0.0;
        double s_x1y = 0.0, s_x2y = 0.0;

        const auto o1 = data_.london_gold_close_history.size() - n;
        const auto o2 = data_.fx_rate_history.size() - n;
        const auto oy = data_.etf_close_history.size() - n;

        for (std::size_t i = 0; i < n; ++i) {
            const double x1 = data_.london_gold_close_history[o1 + i];
            const double x2 = data_.fx_rate_history[o2 + i];
            const double y = data_.etf_close_history[oy + i];
            s_x1 += x1;
            s_x2 += x2;
            s_y += y;
            s_x1x1 += x1 * x1;
            s_x2x2 += x2 * x2;
            s_x1x2 += x1 * x2;
            s_x1y += x1 * y;
            s_x2y += x2 * y;
        }

        std::array<std::array<double, 4>, 3> m{{
            {{static_cast<double>(n), s_x1, s_x2, s_y}},
            {{s_x1, s_x1x1, s_x1x2, s_x1y}},
            {{s_x2, s_x1x2, s_x2x2, s_x2y}},
        }};

        for (int col = 0; col < 3; ++col) {
            int pivot = col;
            for (int row = col + 1; row < 3; ++row) {
                if (std::abs(m[row][col]) > std::abs(m[pivot][col])) {
                    pivot = row;
                }
            }
            if (std::abs(m[pivot][col]) < REGRESSION_SINGULAR_THRESHOLD) {
                return;
            }
            std::swap(m[col], m[pivot]);
            const double div = m[col][col];
            for (int k = col; k < 4; ++k) {
                m[col][k] /= div;
            }
            for (int row = 0; row < 3; ++row) {
                if (row == col) {
                    continue;
                }
                const double factor = m[row][col];
                for (int k = col; k < 4; ++k) {
                    m[row][k] -= factor * m[col][k];
                }
            }
        }

        reg_param_.intercept = m[0][3];
        reg_param_.slope = m[1][3];
        reg_param_.fx_slope = m[2][3];
    }

    bool IsRSIBuyConfirmed() const {
        if (data_.rsi <= RSI_EXTREME_OVERSOLD) {
            return true;
        }
        if (data_.rsi_history.size() < CONFIRM_KLINE_COUNT) {
            return false;
        }
        const auto s = data_.rsi_history.size();
        return data_.rsi <= param_.RSI_Buy_Threshold
            && data_.rsi_history[s - 1] <= data_.rsi_history[s - 2];
    }

    bool IsRSISellConfirmed() const {
        if (data_.rsi_history.size() < CONFIRM_KLINE_COUNT) {
            return false;
        }
        const auto s = data_.rsi_history.size();
        return data_.rsi >= param_.RSI_Sell_Threshold
            && data_.rsi_history[s - 1] >= data_.rsi_history[s - 2];
    }

    static std::vector<std::size_t> FindValleys(const std::deque<double>& v, std::size_t lookback) {
        std::vector<std::size_t> out;
        if (v.size() < 3) {
            return out;
        }
        const auto start = v.size() > lookback ? v.size() - lookback : 1;
        for (std::size_t i = start; i + 1 < v.size(); ++i) {
            if ((v[i] < v[i - 1] && v[i] <= v[i + 1]) || (v[i] <= v[i - 1] && v[i] < v[i + 1])) {
                out.push_back(i);
            }
        }
        return out;
    }

    static std::vector<std::size_t> FindPeaks(const std::deque<double>& v, std::size_t lookback) {
        std::vector<std::size_t> out;
        if (v.size() < 3) {
            return out;
        }
        const auto start = v.size() > lookback ? v.size() - lookback : 1;
        for (std::size_t i = start; i + 1 < v.size(); ++i) {
            if ((v[i] > v[i - 1] && v[i] >= v[i + 1]) || (v[i] >= v[i - 1] && v[i] > v[i + 1])) {
                out.push_back(i);
            }
        }
        return out;
    }

    bool HasBullishDivergence() const {
        if (data_.etf_close_history.size() < DIVERGENCE_PERIOD || data_.rsi_history.size() < DIVERGENCE_PERIOD) {
            return false;
        }
        const auto valleys = FindValleys(data_.etf_close_history, DIVERGENCE_PERIOD + 3);
        if (valleys.size() < 2) {
            return false;
        }
        const auto i1 = valleys[valleys.size() - 2];
        const auto i2 = valleys[valleys.size() - 1];
        if (i1 >= data_.rsi_history.size() || i2 >= data_.rsi_history.size()) {
            return false;
        }
        const double p1 = data_.etf_close_history[i1];
        const double p2 = data_.etf_close_history[i2];
        const double r1 = data_.rsi_history[i1];
        const double r2 = data_.rsi_history[i2];
        return p2 <= p1 * DIVERGENCE_PRICE_BUFFER && r2 > r1;
    }

    bool HasBearishDivergence() const {
        if (data_.etf_close_history.size() < DIVERGENCE_PERIOD || data_.rsi_history.size() < DIVERGENCE_PERIOD) {
            return false;
        }
        const auto peaks = FindPeaks(data_.etf_close_history, DIVERGENCE_PERIOD + 3);
        if (peaks.size() < 2) {
            return false;
        }
        const auto i1 = peaks[peaks.size() - 2];
        const auto i2 = peaks[peaks.size() - 1];
        if (i1 >= data_.rsi_history.size() || i2 >= data_.rsi_history.size()) {
            return false;
        }
        const double p1 = data_.etf_close_history[i1];
        const double p2 = data_.etf_close_history[i2];
        const double r1 = data_.rsi_history[i1];
        const double r2 = data_.rsi_history[i2];
        return p2 >= p1 / DIVERGENCE_PRICE_BUFFER && r2 < r1;
    }

    double GetDynamicPremiumBuyThreshold() const {
        if (data_.premium_history.empty()) {
            return -0.002;
        }
        const auto n = std::min<std::size_t>(PREMIUM_HISTORY_PERIOD, data_.premium_history.size());
        const auto start = data_.premium_history.end() - static_cast<std::ptrdiff_t>(n);
        const double mean = std::accumulate(start, data_.premium_history.end(), 0.0) / static_cast<double>(n);
        double var = 0.0;
        for (auto it = start; it != data_.premium_history.end(); ++it) {
            const double d = *it - mean;
            var += d * d;
        }
        const double sd = std::sqrt(var / static_cast<double>(n));
        return mean - PREMIUM_STD_TIMES * sd;
    }

    double GetDynamicPremiumSellThreshold() const {
        if (data_.premium_history.empty()) {
            return 0.002;
        }
        const auto n = std::min<std::size_t>(PREMIUM_HISTORY_PERIOD, data_.premium_history.size());
        const auto start = data_.premium_history.end() - static_cast<std::ptrdiff_t>(n);
        const double mean = std::accumulate(start, data_.premium_history.end(), 0.0) / static_cast<double>(n);
        double var = 0.0;
        for (auto it = start; it != data_.premium_history.end(); ++it) {
            const double d = *it - mean;
            var += d * d;
        }
        const double sd = std::sqrt(var / static_cast<double>(n));
        return mean + PREMIUM_STD_TIMES * sd;
    }

    bool IsDataValid() const {
        if (!IsFinitePositive(data_.london_gold_price) || !IsFinitePositive(data_.etf_price)
            || !IsFinitePositive(data_.fx_rate) || !IsFinitePositive(data_.volume)) {
            return false;
        }
        if (!std::isfinite(data_.premium) || !std::isfinite(data_.rsi) || !std::isfinite(data_.atr)
            || !std::isfinite(data_.volatility) || !std::isfinite(data_.adx_60m)
            || !std::isfinite(data_.ma20_slope_60m)) {
            return false;
        }
        if (data_.volume < MIN_TRADE_VOLUME) {
            return false;
        }
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        return std::llabs(now - data_.timestamp) <= TIME_SYNC_THRESHOLD;
    }

    double GetATRStopLoss(bool is_buy) const {
        const double atr = data_.atr > 0 ? data_.atr : (data_.atr_history.empty() ? 0.0 : data_.atr_history.back());
        if (is_buy) {
            return data_.etf_price - param_.ATR_StopMultiplier * atr;
        }
        return data_.etf_price + param_.ATR_StopMultiplier * atr;
    }

    void SendAlert(const std::string& msg) {
        if (alert_cb_) {
            alert_cb_(msg);
        }
    }

    bool IsFridayCutoff() const {
        if (data_.timestamp <= 0) {
            return false;
        }
        std::time_t ts = static_cast<std::time_t>(data_.timestamp);
#if defined(_WIN32)
        std::tm local_tm{};
        if (localtime_s(&local_tm, &ts) != 0) {
            return false;
        }
#else
        std::tm local_tm{};
        if (localtime_r(&ts, &local_tm) == nullptr) {
            return false;
        }
#endif
        return local_tm.tm_wday == 5
            && (local_tm.tm_hour > FRIDAY_CUTOFF_HOUR
                || (local_tm.tm_hour == FRIDAY_CUTOFF_HOUR && local_tm.tm_min >= FRIDAY_CUTOFF_MIN));
    }

    bool Buy() {
        const double max_share = (param_.TotalCapital * param_.MaxPositionRatio) / std::max(data_.etf_price, MIN_VALUE_EPSILON);
        if (position_share_ >= max_share) {
            return false;
        }
        const double qty = std::min(param_.T_Share, max_share - position_share_);
        if (qty <= 0.0) {
            return false;
        }
        const double cost = qty * data_.etf_price;
        avg_cost_ = (position_share_ * avg_cost_ + cost) / (position_share_ + qty);
        position_share_ += qty;
        daily_stat_.trade_count++;
        daily_stat_.last_buy_time = data_.timestamp;
        last_trade_ts_ = data_.timestamp;
        if (buy_cb_) {
            buy_cb_(qty);
        }
        return true;
    }

    bool Sell() {
        if (position_share_ <= 0.0) {
            return false;
        }
        const double qty = std::min(param_.T_Share, position_share_);
        const double pnl = qty * (data_.etf_price - avg_cost_);
        daily_stat_.daily_pnl += pnl;
        if (pnl < 0) {
            daily_stat_.consecutive_loss++;
        } else {
            daily_stat_.consecutive_loss = 0;
        }
        position_share_ -= qty;
        if (position_share_ <= MIN_VALUE_EPSILON) {
            position_share_ = 0.0;
            avg_cost_ = 0.0;
        }
        daily_stat_.trade_count++;
        daily_stat_.last_sell_time = data_.timestamp;
        last_trade_ts_ = data_.timestamp;
        if (sell_cb_) {
            sell_cb_(qty);
        }
        return true;
    }

    bool SellAll() {
        if (position_share_ <= 0.0) {
            return false;
        }
        const double qty = position_share_;
        const double pnl = qty * (data_.etf_price - avg_cost_);
        daily_stat_.daily_pnl += pnl;
        if (pnl < 0) {
            daily_stat_.consecutive_loss++;
        } else {
            daily_stat_.consecutive_loss = 0;
        }
        position_share_ = 0.0;
        avg_cost_ = 0.0;
        daily_stat_.trade_count++;
        daily_stat_.last_sell_time = data_.timestamp;
        last_trade_ts_ = data_.timestamp;
        if (sell_all_cb_) {
            sell_all_cb_();
        }
        return true;
    }

    void ExecuteTrade(TradeSignal signal) {
        switch (signal) {
        case TradeSignal::SIGNAL_BUY_T:
            Buy();
            break;
        case TradeSignal::SIGNAL_SELL_T:
            Sell();
            break;
        case TradeSignal::SIGNAL_SELL_ALL:
            SellAll();
            break;
        default:
            break;
        }
    }

    TradeSignal GenerateTradeSignal() {
        if (!IsDataValid() || daily_stat_.is_suspended) {
            return TradeSignal::SIGNAL_HOLD;
        }

        if (data_.volatility >= VOLATILITY_BREAK && data_.rsi <= RSI_EXTREME_OVERSOLD) {
            daily_stat_.is_suspended = true;
            SendAlert("Volatility breaker triggered, trading suspended");
            return TradeSignal::SIGNAL_HOLD;
        }

        market_state_ = GetMarketState();
        UpdateRSIThreshold(market_state_);

        if (IsFridayCutoff()) {
            ExecuteTrade(TradeSignal::SIGNAL_SELL_ALL);
            return TradeSignal::SIGNAL_SELL_ALL;
        }

        if (daily_stat_.trade_count >= MAX_DAILY_TRADE) {
            return TradeSignal::SIGNAL_HOLD;
        }
        if (daily_stat_.consecutive_loss >= MAX_CONSECUTIVE_LOSS) {
            daily_stat_.is_suspended = true;
            SendAlert("Consecutive loss breaker triggered");
            return TradeSignal::SIGNAL_HOLD;
        }
        if (last_trade_ts_ > 0 && (data_.timestamp - last_trade_ts_) < TRADE_COOLDOWN_SEC) {
            return TradeSignal::SIGNAL_HOLD;
        }

        if (data_.is_kline_closed) {
            UpdateMappingByRegression();
        }

        const double premium_buy = GetDynamicPremiumBuyThreshold();
        const double premium_sell = GetDynamicPremiumSellThreshold();
        const double mapped_etf = reg_param_.intercept
            + reg_param_.slope * data_.london_gold_price
            + reg_param_.fx_slope * data_.fx_rate;
        const double predicted_edge = data_.etf_price - mapped_etf;
        const double recent_low = GetRecentLow();
        const double recent_high = GetRecentHigh();

        TradeSignal signal = TradeSignal::SIGNAL_HOLD;
        const bool buy_div = HasBullishDivergence();
        const bool sell_div = HasBearishDivergence();

        if ((IsRSIBuyConfirmed() && data_.premium <= premium_buy && data_.london_gold_price <= recent_low * 1.01)
            || (buy_div && predicted_edge < 0.0)) {
            signal = TradeSignal::SIGNAL_BUY_T;
        } else if ((IsRSISellConfirmed() && data_.premium >= premium_sell && data_.london_gold_price >= recent_high * 0.99)
                   || (sell_div && predicted_edge > 0.0)
                   || data_.etf_price <= GetATRStopLoss(true)) {
            signal = position_share_ > 0.0 ? TradeSignal::SIGNAL_SELL_T : TradeSignal::SIGNAL_HOLD;
        }

        ExecuteTrade(signal);
        return signal;
    }

private:
    std::mutex mu_;
    TradeParam param_;
    MarketData data_;
    DailyStat daily_stat_;
    LinearRegParam reg_param_;
    MarketState market_state_ = MarketState::RANGING;
    TradeSignal current_signal_ = TradeSignal::SIGNAL_HOLD;
    double position_share_ = 0.0;
    double avg_cost_ = 0.0;
    std::int64_t last_trade_ts_ = 0;
    TradeCallback buy_cb_;
    TradeCallback sell_cb_;
    SellAllCallback sell_all_cb_;
    AlertCallback alert_cb_;
};

Engine& GetEngine() {
    static Engine engine;
    return engine;
}

}  // namespace

void SmartQuant_Initialize(const TradeParam& param) {
    GetEngine().Initialize(param);
}

void SmartQuant_Destroy() {
    GetEngine().Destroy();
}

void SmartQuant_UpdateMarketData(const MarketData& data) {
    GetEngine().UpdateMarketData(data);
}

TradeSignal SmartQuant_GetSignal() {
    return GetEngine().GetSignal();
}

void SmartQuant_RegisterBuyCallback(TradeCallback cb) {
    GetEngine().RegisterBuy(std::move(cb));
}

void SmartQuant_RegisterSellCallback(TradeCallback cb) {
    GetEngine().RegisterSell(std::move(cb));
}

void SmartQuant_RegisterSellAllCallback(SellAllCallback cb) {
    GetEngine().RegisterSellAll(std::move(cb));
}

void SmartQuant_RegisterAlertCallback(AlertCallback cb) {
    GetEngine().RegisterAlert(std::move(cb));
}

void SmartQuant_DailyCloseRiskCheck() {
    GetEngine().DailyCloseRiskCheck();
}

EngineStatus SmartQuant_GetStatus() {
    return GetEngine().GetStatus();
}

}  // namespace SmartQuant
