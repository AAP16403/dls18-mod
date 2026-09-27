#ifndef DLS18_CAREER_MARKET_API_H
#define DLS18_CAREER_MARKET_API_H

#include <stdint.h>

enum CareerMarketOfferStatus {
    CM_OFFER_FREE = 0,
    CM_OFFER_WAIT_USER_FEE = 1,
    CM_OFFER_WAIT_USER_WAGE = 2,
    CM_OFFER_WAIT_USER_SELLER = 3,
    CM_OFFER_WAIT_USER_SELLER_COUNTER = 4,
    CM_OFFER_COMPLETED = 5,
    CM_OFFER_REJECTED = 6,
    CM_OFFER_EXPIRED = 7
};

enum CareerMarketDecision {
    CM_DECISION_REJECT = 0,
    CM_DECISION_ACCEPT = 1,
    CM_DECISION_COUNTER = 2
};

enum CareerMarketFinanceCategory {
    CM_FINANCE_OPENING = 1,
    CM_FINANCE_REVENUE = 2,
    CM_FINANCE_WAGE_COST = 3,
    CM_FINANCE_TRANSFER_ALLOCATION = 4,
    CM_FINANCE_PURCHASE = 5,
    CM_FINANCE_SALE = 6,
    CM_FINANCE_CONTRACT_RENEWAL = 7,
    CM_FINANCE_INVESTMENT = 8
};

typedef struct {
    int32_t team_id;
    int32_t cash;
    int32_t transfer_budget;
    int32_t wage_budget;
    int32_t payroll;
    int32_t squad_value;
    int32_t strength;
    uint32_t window_flags;
} CareerMarketClubView;

typedef struct {
    int32_t offer_id;
    int32_t season;
    int32_t turn;
    int32_t window_id;
    int32_t player_id;
    int32_t seller_id;
    int32_t buyer_id;
    int32_t fee;
    int32_t counter_fee;
    int32_t annual_wage;
    int32_t counter_wage;
    int32_t contract_years;
    int32_t rounds;
    int32_t status;
} CareerMarketOfferView;

typedef struct {
    int32_t season;
    int32_t turn;
    int32_t player_id;
    int32_t seller_id;
    int32_t buyer_id;
    int32_t fee;
    int32_t annual_wage;
    int32_t contract_years;
} CareerMarketTransferView;

typedef struct {
    int32_t player_id;
    int32_t club_id;
    int32_t position;
    int32_t rating;
    int32_t value;
    int32_t annual_wage;
} CareerMarketPlayerView;

typedef struct {
    int32_t season;
    int32_t turn;
    int32_t category;
    int32_t cash_delta;
    int32_t transfer_budget_delta;
    int32_t payroll_delta;
    int32_t player_id;
} CareerMarketFinanceEntryView;

/* The UI bridge can call these after loading libCareerMarket.so by dlsym. */
void career_market_on_turn(void *season, void *context, uint32_t base);
int32_t career_market_get_window(void);
int32_t career_market_get_club(int32_t team_id, CareerMarketClubView *out);
int32_t career_market_get_finance_count(int32_t team_id);
int32_t career_market_get_finance_entry(int32_t team_id, int32_t newest_index,
                                        CareerMarketFinanceEntryView *out);
int32_t career_market_get_offer_count(void);
int32_t career_market_get_offer(int32_t active_index, CareerMarketOfferView *out);
int32_t career_market_get_history_count(void);
int32_t career_market_get_history(int32_t index, CareerMarketTransferView *out);
int32_t career_market_get_player_count(void);
int32_t career_market_get_player(int32_t index, CareerMarketPlayerView *out);
int32_t career_market_submit_user_bid(uint32_t base, int32_t player_id, int32_t fee,
                                      int32_t annual_wage, int32_t contract_years);
int32_t career_market_respond_to_offer(uint32_t base, int32_t offer_id,
                                       int32_t decision, int32_t amount);

#endif
