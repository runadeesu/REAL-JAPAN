#pragma once
// Double-entry money ledger. Money circulates between accounts; it is never
// created or destroyed implicitly. The only sources/sinks are explicit
// External accounts (imports/exports, the world outside the simulation) and
// the Government account (which may run a deficit).
//
// Invariant (tested): sum of all balances, including External, is always 0.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rj::econ {

using AccountId = uint64_t;
using Yen = int64_t;

enum class AccountKind : uint8_t { Person, Household, Business, Government, Utility, External };

enum class TxCategory : uint8_t {
  Wage, Purchase, Rent, Electricity, Water, Gas, Telecom, Fuel, Fare, Insurance, Tax,
  Transfer, InitialEndowment, Import, Export, Refund
};
std::string_view categoryName(TxCategory c);

struct Account {
  AccountId id = 0;
  AccountKind kind = AccountKind::Person;
  std::string name;
  Yen balance = 0;
  Yen credit_limit = 0;  // how far below zero this account may go
};

struct Transaction {
  uint64_t seq = 0;
  int64_t unix = 0;
  AccountId from = 0, to = 0;
  Yen amount = 0;
  TxCategory category = TxCategory::Transfer;
  std::string memo;
};

enum class TxResult : uint8_t { Ok, InsufficientFunds, UnknownAccount, InvalidAmount };

class Ledger {
 public:
  Ledger();

  AccountId open(AccountKind kind, std::string name, Yen credit_limit = 0);
  // Endow an account from the External world account (e.g. starting savings).
  TxResult endow(AccountId to, Yen amount, int64_t unix, std::string memo = "initial endowment");
  TxResult transfer(AccountId from, AccountId to, Yen amount, TxCategory cat, int64_t unix,
                    std::string memo = {});

  const Account* account(AccountId id) const;
  Yen balance(AccountId id) const;
  AccountId externalAccount() const { return external_; }
  // Sum over every account including External. Must be 0.
  Yen sumAll() const;
  // Money held inside the simulation (everything except External).
  Yen moneyInWorld() const;
  const std::vector<Transaction>& journal() const { return journal_; }

 private:
  AccountId next_id_ = 1;
  AccountId external_ = 0;
  uint64_t seq_ = 0;
  std::unordered_map<AccountId, Account> accounts_;
  std::vector<Transaction> journal_;
};

// Recurring obligations (salary, rent, utilities, subscriptions ...).
struct RecurringPayment {
  AccountId from = 0, to = 0;
  Yen amount = 0;
  TxCategory category = TxCategory::Transfer;
  int64_t next_due_unix = 0;
  int64_t period_s = 30 * 86400;
  std::string memo;
  int missed = 0;  // arrears count
};

class RecurringScheduler {
 public:
  size_t add(RecurringPayment p);
  // Execute everything due up to `now`. Returns number of failed payments.
  int run(Ledger& ledger, int64_t now_unix);
  const std::vector<RecurringPayment>& items() const { return items_; }

 private:
  std::vector<RecurringPayment> items_;
};

// Payroll with a configurable withholding rate paid to the government.
// GAME RULE, not a model of Japanese tax law (TODO: 源泉徴収税額表 / 社会保険).
TxResult payWage(Ledger& ledger, AccountId employer, AccountId employee, AccountId government,
                 Yen gross, double withholding_rate, int64_t unix);

}  // namespace rj::econ
