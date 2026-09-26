#include "rj/econ/ledger.hpp"

#include <cmath>
#include <limits>

namespace rj::econ {

std::string_view categoryName(TxCategory c) {
  switch (c) {
    case TxCategory::Wage: return "wage";
    case TxCategory::Purchase: return "purchase";
    case TxCategory::Rent: return "rent";
    case TxCategory::Electricity: return "electricity";
    case TxCategory::Water: return "water";
    case TxCategory::Gas: return "gas";
    case TxCategory::Telecom: return "telecom";
    case TxCategory::Fuel: return "fuel";
    case TxCategory::Fare: return "fare";
    case TxCategory::Insurance: return "insurance";
    case TxCategory::Tax: return "tax";
    case TxCategory::Transfer: return "transfer";
    case TxCategory::InitialEndowment: return "initial_endowment";
    case TxCategory::Import: return "import";
    case TxCategory::Export: return "export";
    case TxCategory::Refund: return "refund";
  }
  return "?";
}

Ledger::Ledger() {
  external_ = open(AccountKind::External, "external_world", std::numeric_limits<Yen>::max() / 4);
}

AccountId Ledger::open(AccountKind kind, std::string name, Yen credit_limit) {
  const AccountId id = next_id_++;
  accounts_[id] = Account{id, kind, std::move(name), 0, credit_limit};
  return id;
}

TxResult Ledger::endow(AccountId to, Yen amount, int64_t unix, std::string memo) {
  return transfer(external_, to, amount, TxCategory::InitialEndowment, unix, std::move(memo));
}

TxResult Ledger::transfer(AccountId from, AccountId to, Yen amount, TxCategory cat, int64_t unix,
                          std::string memo) {
  if (amount <= 0) return TxResult::InvalidAmount;
  auto f = accounts_.find(from);
  auto t = accounts_.find(to);
  if (f == accounts_.end() || t == accounts_.end() || from == to) return TxResult::UnknownAccount;
  if (f->second.balance - amount < -f->second.credit_limit) return TxResult::InsufficientFunds;
  f->second.balance -= amount;
  t->second.balance += amount;
  journal_.push_back({++seq_, unix, from, to, amount, cat, std::move(memo)});
  return TxResult::Ok;
}

const Account* Ledger::account(AccountId id) const {
  auto it = accounts_.find(id);
  return it == accounts_.end() ? nullptr : &it->second;
}

Yen Ledger::balance(AccountId id) const {
  const Account* a = account(id);
  return a ? a->balance : 0;
}

Yen Ledger::sumAll() const {
  Yen s = 0;
  for (const auto& [id, a] : accounts_) s += a.balance;
  return s;
}

Yen Ledger::moneyInWorld() const { return sumAll() - balance(external_); }

size_t RecurringScheduler::add(RecurringPayment p) {
  items_.push_back(std::move(p));
  return items_.size() - 1;
}

int RecurringScheduler::run(Ledger& ledger, int64_t now) {
  int failed = 0;
  for (auto& p : items_) {
    while (p.next_due_unix <= now) {
      if (ledger.transfer(p.from, p.to, p.amount, p.category, p.next_due_unix, p.memo) != TxResult::Ok) {
        ++p.missed;
        ++failed;
      }
      p.next_due_unix += p.period_s;
    }
  }
  return failed;
}

TxResult payWage(Ledger& ledger, AccountId employer, AccountId employee, AccountId government,
                 Yen gross, double withholding_rate, int64_t unix) {
  const Yen withheld = static_cast<Yen>(std::llround(static_cast<double>(gross) * withholding_rate));
  if (ledger.balance(employer) + (ledger.account(employer) ? ledger.account(employer)->credit_limit : 0) < gross)
    return TxResult::InsufficientFunds;
  TxResult r = ledger.transfer(employer, employee, gross - withheld, TxCategory::Wage, unix, "net wage");
  if (r != TxResult::Ok) return r;
  if (withheld > 0) r = ledger.transfer(employer, government, withheld, TxCategory::Tax, unix, "withholding");
  return r;
}

}  // namespace rj::econ
