/*
Copyright 2019 Materialize, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "materialized.h"
#include <algorithm>
#include <chrono>
#include "timing.h"

std::vector<std::string>
mz::showAllSources(pqxx::connection &c) {
    std::vector<std::string> results;
    pqxx::nontransaction w(c);

    auto pqResult = w.exec("SHOW SOURCES");
    auto cols = pqResult.columns();
    if (cols != 1) {
        throw UnexpectedCreateSourcesResult {"Wrong number of columns: " + std::to_string(cols)};
    }

    results.reserve(pqResult.size());
    for (const auto& row: pqResult) {
        results.push_back(row[0].as<std::string>());
    }
    return results;
}

bool
mz::createSource(pqxx::connection &c, const std::string& kafkaUrl, const std::string& registry, const std::string& source) {
    pqxx::nontransaction w(c);
    auto topic = source;
    std::replace(topic.begin(), topic.end(), '_', '.'); // Kafka topics are delimited by periods
    try {
        w.exec0("CREATE SOURCE " + source + " FROM KAFKA BROKER '" + kafkaUrl + "' TOPIC '" + topic + "' FORMAT AVRO USING CONFLUENT SCHEMA REGISTRY '" + registry + "' ENVELOPE DEBEZIUM");
    } catch (const pqxx::sql_error &e) {
        fprintf(stderr, "Possibly temporary error creating source %s: %s\n", source.c_str(), e.what());
        return false;
    }
    return true;
}

void mz::createMaterializedView(pqxx::connection& c, const std::string &name, const std::string &query) {
    pqxx::nontransaction w(c);
    w.exec0("DROP VIEW IF EXISTS " + name);
    w.exec0("CREATE MATERIALIZED VIEW " + name + " AS " + query);
}

mz::PeekResults mz::peekView(pqxx::connection &c, const std::string &name, const std::optional<std::string> &order,
                             std::optional<unsigned> limit) {
    std::string query = "SELECT * FROM " + name;
    if (order) {
        query += " ORDER BY " + order.value();
    }
    if (limit) {
        query += " LIMIT " + std::to_string(limit.value());
    }

    auto [time, results] = timeInvocation([&query, &c]() {
        pqxx::nontransaction w(c);
        try {
            return w.exec(query);
        } catch (const pqxx::sql_error &e) {
            if (!strstr(e.what(), "At least one input has no complete timestamps yet.")) {
                throw;
            } else {
                fprintf(stderr, "WARNING: ignoring \"no complete timestamps\" error.");
                return pqxx::result {};
            }
        }
    });

    return {time, results};
}

const char *mz::UnexpectedCreateSourcesResult::what() const noexcept {
    return what_rendered.c_str();
}

mz::UnexpectedCreateSourcesResult::UnexpectedCreateSourcesResult(const std::string& explanation) {
    what_rendered = std::string {"Unexpected result from CREATE SOURCES: "} + explanation;
}
